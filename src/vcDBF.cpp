#include "udResult.h"
#include "udStringUtil.h"
#include "udChunkedArray.h"

#include "vcDBF.h"
#include "vcModals.h"

#include <time.h>
#include <unordered_map>

enum vcDBF_FieldTypes
{
  vcDBF_Character = 'C',
  vcDBF_Date = 'D',
  vcDBF_Float = 'F',
  vcDBF_Numeric = 'N',
  vcDBF_Logical = 'L',
  vcDBF_DateTime = 'T',
  vcDBF_Integer = 'I',
  vcDBF_Currency = 'Y',
  vcDBF_Memo = 'M',
  vcDBF_Count = 9
};

struct vcDBF_Header
{
  int8_t flags;
  int8_t YMD[3];
  int32_t recordCount;
  int16_t headerBytes;
  int16_t recordBytes;
  //int8_t reserved[20];
};

struct vcDBF_MemoBlock
{
  uint32_t type; // ?
  uint32_t length;
  char *pMemo;
};

struct vcDBF_FieldDesc
{
  char fieldName[11];
  char fieldType;
  //uint32_t reserved;
  uint8_t fieldLen;
  uint8_t fieldCount;
  //char reserved[2];
  uint8_t workAreaID;
  //char reserved[2];
  uint8_t setFieldsFlag;
  //char reserved[8];

  vcDBF_DataMappedType mappedType;
};

struct vcDBF
{
  vcDBF_Header header;

  uint8_t fieldCount;
  vcDBF_FieldDesc *pFields;

  udChunkedArray<vcDBF_Record> records;

  bool memo;
  std::unordered_map<uint32_t, vcDBF_MemoBlock> memos;
  struct
  {
    uint16_t memoBlockSize;
    uint32_t newIndex;
    uint32_t firstIndex;
  } memoData;
};

udResult vcDBF_ReadRecord(vcDBF *pDBF, udFile *pFile);
udResult vcDBF_WriteRecord(vcDBF *pDBF, udFile *pFile, vcDBF_Record *pRecord, udFile *pMemo = nullptr);

const char spaceBuffer[255] = { ' ' };

template <typename T>
void vcDBF_FlipEndian(T *pData)
{
  uint8_t bytes[sizeof(T)];
  memcpy(bytes, pData, sizeof(T));

  for (uint32_t i = 0; i < (sizeof(T) / 2); ++i)
  {
    uint8_t temp = bytes[i];
    bytes[i] = bytes[sizeof(T) - i - 1];
    bytes[sizeof(T) - i - 1] = temp;
  }

  memcpy(pData, bytes, sizeof(T));
};

udResult vcDBF_Create(vcDBF **ppDBF)
{
  if (ppDBF == nullptr)
    return udR_InvalidParameter_;

  udResult result = udR_Failure_;

  vcDBF *pDBF = udAllocType(vcDBF, 1, udAF_Zero);
  UD_ERROR_NULL(pDBF, udR_MemoryAllocationFailure);

  pDBF->records.Init(32);

  *ppDBF = pDBF;

  result = udR_Success;

epilogue:

  return result;
}

udResult vcDBF_Destroy(vcDBF **ppDBF)
{
  if (ppDBF == nullptr || *ppDBF == nullptr)
    return udR_InvalidParameter_;

  if ((*ppDBF)->memo)
  {
    for (std::pair<uint32_t, vcDBF_MemoBlock> kvp : (*ppDBF)->memos)
      udFree(kvp.second.pMemo);

    (*ppDBF)->memos.clear();
  }

  for (uint32_t j = 0; j < (*ppDBF)->fieldCount; ++j)
  {
    if ((*ppDBF)->pFields[j].mappedType != vcDBFDMT_String)
      continue;

    for (uint32_t i = 0; i < (uint32_t)(*ppDBF)->records.length; ++i)
      udFree((*ppDBF)->records[i].pFields[j].pString);
  }

  for (uint32_t i = 0; i < (uint32_t)(*ppDBF)->records.length; ++i)
    udFree((*ppDBF)->records[i].pFields);

  (*ppDBF)->records.Deinit();
  udFree((*ppDBF)->pFields);
  udFree(*ppDBF);

  *ppDBF = nullptr;

  return udR_Success;
}

udResult vcDBF_ReadHeader(udFile *pFile, vcDBF_Header *pHeader)
{
  udResult result = udR_Failure_;

  UD_ERROR_CHECK(udFile_Read(pFile, &pHeader->flags, sizeof(pHeader->flags)));
  UD_ERROR_IF((pHeader->flags & 0b00000011) != 3, udR_VersionMismatch); // DBase IV

  UD_ERROR_CHECK(udFile_Read(pFile, pHeader->YMD, sizeof(pHeader->YMD)));
  UD_ERROR_IF(pHeader->YMD[0] < 0 || pHeader->YMD[1] > 12 || pHeader->YMD[2] > 31, udR_ParseError);

  UD_ERROR_CHECK(udFile_Read(pFile, &pHeader->recordCount, sizeof(pHeader->recordCount)));
  UD_ERROR_IF(pHeader->recordCount < 1, udR_NothingToDo);

  UD_ERROR_CHECK(udFile_Read(pFile, &pHeader->headerBytes, sizeof(pHeader->headerBytes)));
  UD_ERROR_IF(pHeader->headerBytes < 31, udR_NothingToDo);

  UD_ERROR_CHECK(udFile_Read(pFile, &pHeader->recordBytes, sizeof(pHeader->recordBytes)));
  UD_ERROR_IF(pHeader->recordBytes < 1, udR_NothingToDo);

  int8_t trash[20];
  UD_ERROR_CHECK(udFile_Read(pFile, &trash, sizeof(uint8_t) * 20)); // Seek forward, 20 reserved bytes

  result = udR_Success;

epilogue:

  return result;
}

udResult vcDBF_ReadFieldDesc(udFile *pFile, vcDBF_FieldDesc *pDesc)
{
  if (pFile == nullptr)
    return udR_InvalidParameter_;

  udResult result = udR_Failure_;

  vcDBF_DataMappedType type = vcDBFDMT_Invalid;
  uint8_t trash[10];

  // All in-use fields are single byte sized, no endianness concerns
  UD_ERROR_CHECK(udFile_Read(pFile, pDesc->fieldName, sizeof(char)));

  UD_ERROR_IF(pDesc->fieldName[0] == '\x0d', udR_NothingToDo); // Field terminator

  UD_ERROR_CHECK(udFile_Read(pFile, &pDesc->fieldName[1], sizeof(pDesc->fieldName) - sizeof(pDesc->fieldName[1])));
  UD_ERROR_CHECK(udFile_Read(pFile, &pDesc->fieldType, sizeof(char)));

  UD_ERROR_CHECK(udFile_Read(pFile, &trash, sizeof(uint32_t)));

  UD_ERROR_CHECK(udFile_Read(pFile, &pDesc->fieldLen, sizeof(uint8_t)));
  UD_ERROR_CHECK(udFile_Read(pFile, &pDesc->fieldCount, sizeof(uint8_t)));

  UD_ERROR_CHECK(udFile_Read(pFile, &trash, sizeof(uint16_t)));

  UD_ERROR_CHECK(udFile_Read(pFile, &pDesc->workAreaID, sizeof(uint8_t)));

  UD_ERROR_CHECK(udFile_Read(pFile, &trash, sizeof(uint8_t) * 10));

  UD_ERROR_CHECK(udFile_Read(pFile, &pDesc->setFieldsFlag, sizeof(uint8_t)));

  switch (pDesc->fieldType)
  {
  case vcDBF_Character: // Falls through
  case vcDBF_Date:
  case vcDBF_DateTime:
    type = vcDBFDMT_String;
    break;

  case vcDBF_Float: // Falls through
  case vcDBF_Currency:
  case vcDBF_Numeric:
    type = vcDBFDMT_Float;
    break;

  case vcDBF_Logical:
    type = vcDBFDMT_Logical;
    break;

  case vcDBF_Integer:
    type = vcDBFDMT_Integer;
    break;

  case vcDBF_Memo:
    type = vcDBFDMT_Memo;
  }

  UD_ERROR_IF(type == vcDBFDMT_Invalid, udR_ParseError);
  pDesc->mappedType = type;

  result = udR_Success;

epilogue:

  return result;
}

udResult vcDBF_LoadMemoFile(vcDBF *pDBF, const char *pFilename)
{
  if (pDBF == nullptr || pFilename == nullptr)
    return udR_InvalidParameter_;

  udResult result = udR_Failure_;

  char *pPos = nullptr, *pData = nullptr;
  int64_t fileLength = 0;
  uint32_t numBlocks = 0;
  uint32_t i;

  UD_ERROR_CHECK(udFile_Load(pFilename, &pData, &fileLength));
  
  pDBF->memoData.newIndex = (uint32_t)*pData;
  vcDBF_FlipEndian(&pDBF->memoData.newIndex);

  pDBF->memoData.memoBlockSize = *((uint16_t *)pData + 4);

  pDBF->memoData.firstIndex = (uint32_t)(512 / pDBF->memoData.memoBlockSize + .5); // Round up

  numBlocks = (uint32_t)fileLength / pDBF->memoData.memoBlockSize;

  i = pDBF->memoData.firstIndex;
  for (; i < numBlocks; ++i)
  {
    pPos = pData + i * pDBF->memoData.memoBlockSize;

    vcDBF_MemoBlock memo = {};

    memo.type = *((uint32_t *)pPos);
    memo.length = *((uint32_t *)pPos + 4);
    memo.pMemo = udAllocType(char, memo.length, udAF_Zero);
    memcpy(memo.pMemo, pPos + 8, memo.length);
    
    pDBF->memos[i] = memo;

    i += (uint32_t)memo.length / pDBF->memoData.memoBlockSize;
  }

  pDBF->memoData.newIndex = i;

  result = udR_Success;

epilogue:

  return result;
}

udResult vcDBF_Load(vcDBF **ppDBF, const char *pFilename)
{
  if (ppDBF == nullptr || pFilename == nullptr)
    return udR_InvalidParameter_;

  udResult result = udR_Failure_;

  int fieldCount = 0;
  int headerBytesRemaining = 0;
  uint64_t trash = 0;
  udFile *pFile = nullptr;

  vcDBF *pDBF = udAllocType(vcDBF, 1, udAF_Zero);
  UD_ERROR_NULL(pDBF, udR_MemoryAllocationFailure);

  pDBF->records.Init(32);

  UD_ERROR_CHECK(udFile_Open(&pFile, pFilename, udFOF_Read));

  UD_ERROR_CHECK(vcDBF_ReadHeader(pFile, &pDBF->header));

  if ((pDBF->header.flags & 0b01000000) == 0b01000000) // Memo flag
  {
    uint32_t len = (uint32_t)udStrlen(pFilename);
    char *pDBT = udStrdup(pFilename);
    pDBT[len - 1] = 'T'; // X.DBF --> X.DBT

    UD_ERROR_CHECK(vcDBF_LoadMemoFile(pDBF, pDBT));

    udFree(pDBT);
  }

  fieldCount = pDBF->header.headerBytes / 32; // Header & field descriptors are 32 bytes, overestimate by 1 to make readfielddesc process the field terminator
  UD_ERROR_IF(fieldCount < 1, udR_ParseError);
  pDBF->fieldCount = (uint8_t)udMin(fieldCount, 255);

  pDBF->pFields = udAllocType(vcDBF_FieldDesc, pDBF->fieldCount, udAF_Zero);
  UD_ERROR_NULL(pDBF->pFields, udR_MemoryAllocationFailure);

  headerBytesRemaining = pDBF->header.headerBytes - 32;
  for (uint8_t i = 0; i < pDBF->fieldCount; ++i)
  {
    result = vcDBF_ReadFieldDesc(pFile, &pDBF->pFields[i]);
    if (result == udR_NothingToDo)
    {
      pDBF->fieldCount = i;
      result = udR_Success;
      --headerBytesRemaining;
      break;
    }
    else
    {
      headerBytesRemaining -= 32;
      UD_ERROR_CHECK(result);
    }
  }

  // If we're not at the end of the header, seek to the end
  while (headerBytesRemaining > 0)
  {
    UD_ERROR_CHECK(udFile_Read(pFile, &trash, udMin((int)sizeof(trash), headerBytesRemaining)));
    headerBytesRemaining -= sizeof(trash);
  }
  
  while ((result = vcDBF_ReadRecord(pDBF, pFile)) != udR_NothingToDo)
    UD_ERROR_CHECK(result);

  *ppDBF = pDBF;

  result = udR_Success;

epilogue:

  udFile_Close(&pFile);

  if (result != udR_Success)
    vcDBF_Destroy(&pDBF);

  return result;
}

udResult vcDBF_ReadRecord(vcDBF *pDBF, udFile *pFile)
{
  if (pDBF == nullptr || pFile == nullptr)
    return udR_InvalidParameter_;

  udResult result = udR_Failure_;
  char marker = '\0';
  uint32_t blockIndex = 0;
  vcDBF_Record record = {};

  UD_ERROR_CHECK(udFile_Read(pFile, &marker, sizeof(char)));

  if (marker == '\x2A') // Deleted record
  {
    record.deleted = true;
  }
  else
  {
    record.deleted = false;
    UD_ERROR_IF(marker == '\x1A', udR_NothingToDo); // End of file reached
    UD_ERROR_IF(marker != '\x20', udR_ParseError); // Record marker
  }

  record.pFields = udAllocType(vcDBF_Record::vcDBF_RecordField, pDBF->fieldCount, udAF_Zero);
  UD_ERROR_NULL(record.pFields, udR_MemoryAllocationFailure);

  for (uint32_t i = 0; i < pDBF->fieldCount; ++i)
  {
    uint32_t j = 0;

    char buffer[256];
    UD_ERROR_CHECK(udFile_Read(pFile, buffer, pDBF->pFields[i].fieldLen));
    buffer[pDBF->pFields[i].fieldLen] = '\0';

    // Skip padding to content
    for (; j < pDBF->pFields[i].fieldLen; ++j)
      if (buffer[j] != ' ')
        break;

    // If no content within field length set to null
    if (j == pDBF->pFields[i].fieldLen)
    {
      j = 0;
      buffer[0] = '\0';
    }

    switch (pDBF->pFields[i].mappedType)
    {
    case vcDBFDMT_String: // String types shouldn't be space padded, they're padded with nulls at the end
      record.pFields[i].pString = udStrdup(buffer);
      break;

    case vcDBFDMT_Float:
      record.pFields[i].floaty = udStrAtof(buffer + j);
      break;

    case vcDBFDMT_Logical:
      UD_ERROR_IF(buffer[j] == '?', udR_Success);
      record.pFields[i].logical = (buffer[j] == 'Y' || buffer[j] == 'y' || buffer[j] == 'T' || buffer[j] == 't');

      UD_ERROR_IF(!record.pFields[i].logical && (buffer[j] != 'N' && buffer[j] != 'n' && buffer[j] != 'F' && buffer[j] != 'f'), udR_ParseError);
      break;

    case vcDBFDMT_Integer:
      record.pFields[i].integer = udStrAtoi(buffer + j);
      break;

    case vcDBFDMT_Memo:
      UD_ERROR_IF(!pDBF->memo, udR_ParseError);

      if (pDBF->pFields[i].fieldLen == 4) // memo block index stored as uint32_t
        memcpy(&blockIndex, buffer, sizeof(uint32_t));
      else
        blockIndex = udStrAtoi(buffer + j);

      record.pFields[i].pString = pDBF->memos[blockIndex].pMemo;
      break;

    case vcDBFDMT_Invalid:
      break;
    }
  }

  pDBF->records.PushBack(record);

  result = udR_Success;

epilogue:

  return (uint32_t)pDBF->records.length >= (uint32_t)pDBF->header.recordCount ? udR_NothingToDo : result;
}

udResult vcDBF_WriteFieldDesc(udFile *pFile, const vcDBF_FieldDesc *pDesc)
{
  if (pFile == nullptr)
    return udR_InvalidParameter_;

  udResult result = udR_Failure_;

  uint8_t blank[10] = {};
  uint8_t strLen = 0;
  for (; pDesc->fieldName[strLen] != '\0' && strLen < 11; ++strLen);

  // All these fields are single byte numbers or unused, no need to flip endian
  UD_ERROR_CHECK(udFile_Write(pFile, &pDesc->fieldName, strLen));

  if (strLen < 11)
    UD_ERROR_CHECK(udFile_Write(pFile, blank, 11 - strLen));

  UD_ERROR_CHECK(udFile_Write(pFile, &pDesc->fieldType, sizeof(char)));
  UD_ERROR_CHECK(udFile_Write(pFile, blank, sizeof(uint32_t)));
  UD_ERROR_CHECK(udFile_Write(pFile, &pDesc->fieldLen, sizeof(uint8_t)));
  UD_ERROR_CHECK(udFile_Write(pFile, &pDesc->fieldCount, sizeof(uint8_t)));
  UD_ERROR_CHECK(udFile_Write(pFile, blank, 2));
  UD_ERROR_CHECK(udFile_Write(pFile, &pDesc->workAreaID, sizeof(uint8_t)));
  UD_ERROR_CHECK(udFile_Write(pFile, blank, 2));
  UD_ERROR_CHECK(udFile_Write(pFile, &pDesc->setFieldsFlag, sizeof(uint8_t)));
  UD_ERROR_CHECK(udFile_Write(pFile, blank, 8));

  result = udR_Success;

epilogue:

  return result;
}

udResult vcDBF_WriteMemoFile(vcDBF *pDBF, udFile *pFile)
{
  if (pDBF == nullptr || pFile == nullptr)
    return udR_InvalidParameter_;

  udResult result = udR_Failure_;

  char *pSpaceBuffer = udAllocType(char, pDBF->memoData.memoBlockSize, udAF_None);
  for (int i = 0; i < pDBF->memoData.memoBlockSize; ++i)
    pSpaceBuffer[i] = ' ';

  uint32_t bigIndex = pDBF->memoData.newIndex;
  vcDBF_FlipEndian(&bigIndex);

  UD_ERROR_CHECK(udFile_Write(pFile, &bigIndex, sizeof(uint32_t)));
  UD_ERROR_CHECK(udFile_Write(pFile, &pDBF->memoData.memoBlockSize, sizeof(uint16_t)));
  UD_ERROR_CHECK(udFile_Write(pFile, pSpaceBuffer, pDBF->memoData.memoBlockSize - sizeof(uint16_t) - sizeof(uint32_t)));

  for (uint32_t i = pDBF->memoData.firstIndex; i < pDBF->memos.size(); ++i)
  {
    vcDBF_MemoBlock *pBlock = &pDBF->memos[i];

    UD_ERROR_CHECK(udFile_Write(pFile, &pBlock->type, sizeof(uint32_t)));
    UD_ERROR_CHECK(udFile_Write(pFile, &pBlock->length, sizeof(uint32_t)));
    UD_ERROR_CHECK(udFile_Write(pFile, pBlock->pMemo, pBlock->length));

    UD_ERROR_CHECK(udFile_Write(pFile, pSpaceBuffer, pDBF->memoData.memoBlockSize - (pBlock->length + (sizeof(uint32_t) * 2)) % pDBF->memoData.memoBlockSize));

    i += (uint32_t)(pBlock->length / pDBF->memoData.memoBlockSize);
  }

  result = udR_Success;

epilogue:

  udFree(pSpaceBuffer);

  return result;
}

udResult vcDBF_Save(vcDBF *pDBF, const char *pFilename)
{
  if (pDBF == nullptr || pFilename == nullptr)
    return udR_InvalidParameter_;

  udResult result = udR_Failure_;

  vcDBF_Header dbfh = {};
  time_t currTime = time_t(0);
  tm *pLocalTime;
  uint8_t blank[20] = {};
  udFile *pFile = nullptr, *pMemo = nullptr;

  uint16_t filenameLen = 0;
  char *pMemoname = nullptr;
  const char *pFullFilename = pFilename;
  if (!udStrEndsWithi(pFullFilename, ".DBF"))
    pFullFilename = udTempStr("%s.dbf", pFullFilename);

  filenameLen = (uint16_t)udStrlen(pFullFilename);

  UD_ERROR_CHECK(udFile_Open(&pFile, pFullFilename, udFOF_Write));

  // DBF File header
  dbfh.flags = 3; // TODO: Revisit other flags, check if any are necessary

  if (pDBF->memo) // Or check on disk for a memo file
    dbfh.flags |= 0b10000000;

  dbfh.recordCount = (int32_t)pDBF->records.length;
  dbfh.headerBytes = 32 * (pDBF->fieldCount + 1);

  pLocalTime = localtime(&currTime);

  dbfh.YMD[0] = (int8_t)pLocalTime->tm_year;
  dbfh.YMD[1] = (int8_t)(pLocalTime->tm_mon + 1);
  dbfh.YMD[2] = (int8_t)pLocalTime->tm_mday;

  for (int i = 0; i < pDBF->fieldCount; ++i)
    dbfh.recordBytes += pDBF->pFields[i].fieldLen;

  UD_ERROR_CHECK(udFile_Write(pFile, &dbfh, sizeof(vcDBF_Header)));

  UD_ERROR_CHECK(udFile_Write(pFile, blank, 20)); // Reserved bytes

  for (int i = 0; i < pDBF->fieldCount; ++i)
    UD_ERROR_CHECK(vcDBF_WriteFieldDesc(pFile, &pDBF->pFields[i]));

  UD_ERROR_CHECK(udFile_Write(pFile, "\x0d", sizeof(char)));

  // Memo file
  if (pDBF->memo)
  {
    pMemoname = udAllocType(char, filenameLen + 1, udAF_Zero);

    if (!udStrEndsWithi(pFilename, ".DBF"))
    {
      udSprintf(pMemoname, filenameLen + 1, "%s.dbt", pFilename);
    }
    else
    {
      udStrcpy(pMemoname, filenameLen + 1, pFilename);
      pMemoname[filenameLen - 1] = 't';
    }

    UD_ERROR_CHECK(udFile_Open(&pMemo, pMemoname, udFOF_Write));
    udFree(pMemoname);

    UD_ERROR_CHECK(vcDBF_WriteMemoFile(pDBF, pMemo));
  }

  // Write records
  for (uint32_t i = 0; i < pDBF->records.length; ++i)
  {
    vcDBF_Record *pRecord = &pDBF->records[i];

    if (pRecord->deleted)
      UD_ERROR_CHECK(udFile_Write(pFile, "\x2A", sizeof(char)));
    else
      UD_ERROR_CHECK(udFile_Write(pFile, "\x20", sizeof(char)));

    UD_ERROR_CHECK(vcDBF_WriteRecord(pDBF, pFile, pRecord, pMemo));
  }

  UD_ERROR_CHECK(udFile_Write(pFile, "\x1A", sizeof(char)));

  result = udR_Success;

epilogue:

  return result;
}

udResult vcDBF_FindFieldIndex(vcDBF *pDBF, const char *pFieldName, uint16_t *pIndex)
{
  if (pDBF == nullptr || pFieldName == nullptr || pIndex == nullptr)
    return udR_InvalidParameter_;

  udResult result = udR_Failure_;

  uint16_t i = 0;
  for (; i < pDBF->fieldCount; ++i)
    if (udStrBeginsWithi(pDBF->pFields[i].fieldName, pFieldName) || udStrEndsWithi(pDBF->pFields[i].fieldName, pFieldName))
      break;

  UD_ERROR_IF(i == pDBF->fieldCount, udR_Failure_);
  *pIndex = i;

  result = udR_Success;

epilogue:

  return result;
}

udResult vcDBF_GetFieldIndex(vcDBF *pDBF, const char *pFieldName, uint16_t *pIndex)
{
  if (pDBF == nullptr || pFieldName == nullptr || pIndex == nullptr)
    return udR_InvalidParameter_;

  udResult result = udR_Failure_;

  uint16_t i = 0;
  for (; i < pDBF->fieldCount; ++i)
    if (udStrEquali(pDBF->pFields[i].fieldName, pFieldName))
      break;

  UD_ERROR_IF(i == pDBF->fieldCount, udR_Failure_);
  *pIndex = i;

  result = udR_Success;

epilogue:

  return result;
}

uint16_t vcDBF_GetFieldCount(vcDBF *pDBF)
{
  if (pDBF == nullptr)
    return 0;

  return pDBF->fieldCount;
}

uint32_t vcDBF_GetRecordCount(vcDBF *pDBF)
{
  if (pDBF == nullptr)
    return 0;

  return (uint32_t)pDBF->records.length;
}

udResult vcDBF_AddField(vcDBF *pDBF, char *pFieldName, char fieldType, uint8_t fieldLen)
{
  if (pDBF == nullptr)
    return udR_InvalidParameter_;

  udResult result = udR_Failure_;
  vcDBF_FieldDesc *pDesc = nullptr;

  UD_ERROR_IF(pDBF->records.length > 0, udR_NotAllowed);

  pDesc = udAllocType(vcDBF_FieldDesc, pDBF->fieldCount + 1, udAF_Zero);
  UD_ERROR_NULL(pDesc, udR_MemoryAllocationFailure);
  udStrncpy(pDesc[pDBF->fieldCount].fieldName, 11, pFieldName, 11);
  pDesc[pDBF->fieldCount].fieldType = fieldType;
  pDesc[pDBF->fieldCount].fieldLen = fieldLen;

  memcpy(pDesc, pDBF->pFields, sizeof(vcDBF_FieldDesc) * pDBF->fieldCount);

  ++pDBF->fieldCount;

  udFree(pDBF->pFields);
  pDBF->pFields = pDesc;

  result = udR_Success;

epilogue:

  return result;
}

udResult vcDBF_RemoveField(vcDBF *pDBF, uint16_t fieldIndex)
{
  if (pDBF == nullptr || fieldIndex >= pDBF->fieldCount)
    return udR_InvalidParameter_;

  udResult result = udR_Failure_;

  UD_ERROR_IF(pDBF->records.length > 0, udR_NotAllowed);

  --pDBF->fieldCount;

  // Shift fields back on top of removal target
  for (int i = fieldIndex; i < pDBF->fieldCount; ++i)
    pDBF->pFields[i] = pDBF->pFields[i + 1];
  
  result = udR_Success;

epilogue:

  return result;
}

udResult vcDBF_GetRecord(vcDBF *pDBF, vcDBF_Record **ppRecord, uint32_t recordIndex)
{
  if (pDBF == nullptr)
    return udR_InvalidParameter_;

  udResult result = udR_Failure_;

  UD_ERROR_IF(pDBF->records.length <= recordIndex, udR_OutOfRange);
  UD_ERROR_NULL((*ppRecord = &pDBF->records[recordIndex]), udR_MemoryAllocationFailure);

  result = udR_Success;

epilogue:

  return result;
}

udResult vcDBF_CreateRecord(vcDBF *pDBF, vcDBF_Record **ppRecord)
{
  if (pDBF == nullptr || ppRecord == nullptr)
    return udR_InvalidParameter_;

  udResult result = udR_Failure_;

  vcDBF_Record rec = {};

  rec.pFields = udAllocType(vcDBF_Record::vcDBF_RecordField, pDBF->fieldCount, udAF_Zero);

  pDBF->records.PushBack(rec);

  UD_ERROR_NULL((*ppRecord = &pDBF->records[pDBF->records.length - 1]), udR_MemoryAllocationFailure);

  result = udR_Success;

epilogue:

  return result;
}

udResult vcDBF_DeleteRecord(vcDBF *pDBF, vcDBF_Record *pRecord)
{
  if (pDBF == nullptr || pRecord == nullptr)
    return udR_InvalidParameter_;

  pRecord->deleted = true;

  return udR_Success;
}

udResult vcDBF_DeleteRecord(vcDBF *pDBF, uint32_t recordIndex)
{
  if (pDBF == nullptr)
    return udR_InvalidParameter_;

  udResult result = udR_Failure_;
  vcDBF_Record *pRecord = nullptr;

  UD_ERROR_IF(pDBF->records.length <= recordIndex, udR_OutOfRange);

  pRecord = &pDBF->records[recordIndex];
  UD_ERROR_NULL(pRecord, udR_OutOfRange);

  pRecord->deleted = true;

  result = udR_Success;

epilogue:

  return result;
}

udResult vcDBF_RecordReadFieldBool(vcDBF_Record *pRecord, bool *pValue, uint16_t fieldIndex)
{
  if (pRecord == nullptr || pValue == nullptr)
    return udR_InvalidParameter_;

  *pValue = pRecord->pFields[fieldIndex].logical;

  return udR_Success;
}

udResult vcDBF_RecordReadFieldDouble(vcDBF_Record *pRecord, double *pValue, uint16_t fieldIndex)
{
  if (pRecord == nullptr || pValue == nullptr)
    return udR_InvalidParameter_;

  *pValue = pRecord->pFields[fieldIndex].floaty;

  return udR_Success;
}

udResult vcDBF_RecordReadFieldInt(vcDBF_Record *pRecord, int32_t *pValue, uint16_t fieldIndex)
{
  if (pRecord == nullptr || pValue == nullptr)
    return udR_InvalidParameter_;

  *pValue = pRecord->pFields[fieldIndex].integer;

  return udR_Success;
}

udResult vcDBF_RecordReadFieldString(vcDBF_Record *pRecord, const char **ppValue, uint16_t fieldIndex)
{
  if (pRecord == nullptr || ppValue == nullptr)
    return udR_InvalidParameter_;

  *ppValue = pRecord->pFields[fieldIndex].pString;

  return udR_Success;
}

udResult vcDBF_RecordReadFieldMemo(vcDBF *pDBF, vcDBF_Record *pRecord, const char **ppValue, uint16_t fieldIndex)
{
  if (pRecord == nullptr || ppValue == nullptr|| pDBF == nullptr)
    return udR_InvalidParameter_;

  udResult result = udR_Failure_;

  UD_ERROR_IF(!pDBF->memo, udR_ObjectTypeMismatch);

  *ppValue = pDBF->memos[pRecord->pFields[fieldIndex].integer].pMemo;

  result = udR_Success;

epilogue:

  return result;
}

udResult vcDBF_RecordWriteFieldBool(vcDBF_Record *pRecord, bool value, uint16_t fieldIndex)
{
  if (pRecord == nullptr)
    return udR_InvalidParameter_;

  pRecord->pFields[fieldIndex].logical = value;

  return udR_Success;
}

udResult vcDBF_RecordWriteFieldDouble(vcDBF_Record *pRecord, double value, uint16_t fieldIndex)
{
  if (pRecord == nullptr)
    return udR_InvalidParameter_;

  pRecord->pFields[fieldIndex].floaty = value;

  return udR_Success;
}

udResult vcDBF_RecordWriteFieldInt(vcDBF_Record *pRecord, int32_t value, uint16_t fieldIndex)
{
  if (pRecord == nullptr)
    return udR_InvalidParameter_;

  pRecord->pFields[fieldIndex].integer = value;

  return udR_Success;
}

udResult vcDBF_RecordWriteFieldString(vcDBF_Record *pRecord, const char *pValue, uint16_t fieldIndex)
{
  if (pRecord == nullptr)
    return udR_InvalidParameter_;

  udFree(pRecord->pFields[fieldIndex].pString);

  if (pValue != nullptr)
    pRecord->pFields[fieldIndex].pString = udStrdup(pValue);
  else
    pRecord->pFields[fieldIndex].pString = nullptr;

  return udR_Success;
}

udResult vcDBF_RecordWriteFieldMemo(vcDBF_Record *pRecord, vcDBF *pDBF, const char *pValue, uint16_t fieldIndex)
{
  if (pRecord == nullptr || pDBF == nullptr)
    return udR_InvalidParameter_;

  int32_t blockIndex = -1;

  if (pDBF->pFields[fieldIndex].fieldLen == 4) // Block index stored as int
    blockIndex = pRecord->pFields[fieldIndex].integer;
  else
    blockIndex = udStrAtoi(pRecord->pFields[fieldIndex].pString);

  uint32_t strLen = (uint32_t)udStrlen(pValue);
  vcDBF_MemoBlock *pBlock = &pDBF->memos[blockIndex];

  if (pBlock != nullptr)
  {
    udFree(pBlock->pMemo);

    // This will necessitate later writing until the next memo block index is found
    if ((int)(strLen + 2 / pDBF->memoData.memoBlockSize) <= (int)(pBlock->length + 2 / pDBF->memoData.memoBlockSize)) // 2 terminating characters
    {
      pBlock->pMemo = udStrdup(pValue);

      return udR_Success;
    }

    pDBF->memos.erase(blockIndex);
  }

  // Here we need a new block
  vcDBF_MemoBlock newBlock = {};
  newBlock.length = strLen;
  newBlock.pMemo = udStrdup(pValue);

  pDBF->memos[pDBF->memoData.newIndex] = newBlock;
  pDBF->memoData.newIndex += 1 + (int)(strLen + 2 / pDBF->memoData.memoBlockSize); // 2 terminating characters

  return udR_Success;
}

udResult vcDBF_WriteString(udFile *pFile, const char *pString, uint8_t fieldLen)
{
  if (pFile == nullptr || fieldLen < 1)
    return udR_InvalidParameter_;

  udResult result = udR_Failure_;

  uint32_t outputLen = (uint32_t) udStrlen(pString);

  if (outputLen > fieldLen)
    outputLen = fieldLen; // Write out as much as will fit
  
  UD_ERROR_CHECK(udFile_Write(pFile, pString, outputLen));

  if (outputLen < fieldLen)
    UD_ERROR_CHECK(udFile_Write(pFile, spaceBuffer, fieldLen - outputLen));

  result = udR_Success;

epilogue:

  return result;
}

udResult vcDBF_WriteBool(udFile *pFile, const bool input, uint8_t fieldLen)
{
  if (pFile == nullptr || fieldLen < 1)
    return udR_InvalidParameter_;

  udResult result = udR_Failure_;

  if (fieldLen > 1)
    UD_ERROR_CHECK(udFile_Write(pFile, spaceBuffer, fieldLen - sizeof(char)));

  UD_ERROR_CHECK(udFile_Write(pFile, (input) ? "T" : "F", sizeof(char)));

  result = udR_Success;

epilogue:

  return result;
}

udResult vcDBF_WriteDouble(udFile *pFile, const double input, uint8_t fieldLen)
{
  if (pFile == nullptr || fieldLen < 1)
    return udR_InvalidParameter_;

  udResult result = udR_Failure_;

  uint32_t outputLen = 0;
  char *pOutput = udAllocType(char, fieldLen + 1, udAF_Zero);
  UD_ERROR_NULL(pOutput, udR_MemoryAllocationFailure);

  outputLen = (uint32_t)udStrFtoa(pOutput, fieldLen + 1, input, 5) - 1; //TODO: How much precision?

  if (outputLen > fieldLen)
    outputLen = fieldLen; // Write out as much as will fit
  else if (outputLen < fieldLen)
    UD_ERROR_CHECK(udFile_Write(pFile, spaceBuffer, fieldLen - outputLen));

  UD_ERROR_CHECK(udFile_Write(pFile, pOutput, outputLen));

  result = udR_Success;

epilogue:

  udFree(pOutput);

  return result;
}

udResult vcDBF_WriteInteger(udFile *pFile, const int32_t input, uint8_t fieldLen)
{
  if (pFile == nullptr || fieldLen < 1)
    return udR_InvalidParameter_;

  udResult result = udR_Failure_;

  uint32_t outputLen = 0;
  char *pOutput = udAllocType(char, fieldLen + 1, udAF_Zero);
  UD_ERROR_NULL(pOutput, udR_MemoryAllocationFailure);

  outputLen = (uint32_t)udStrItoa(pOutput, fieldLen + 1, input) - 1;

  if (outputLen > fieldLen)
    outputLen = fieldLen; // Write out as much as will fit
  else if (outputLen < fieldLen)
    UD_ERROR_CHECK(udFile_Write(pFile, spaceBuffer, fieldLen - outputLen));

  UD_ERROR_CHECK(udFile_Write(pFile, pOutput, outputLen));

  result = udR_Success;

epilogue:

  udFree(pOutput);

  return result;
}

udResult vcDBF_WriteMemoBlock(udFile *pMemo, const char *pString, uint16_t blockSize)
{
  if (pMemo == nullptr || pString == nullptr)
    return udR_Failure_;

  udResult result = udR_Failure_;

  uint32_t strLen = (uint32_t)udStrlen(pString);

  UD_ERROR_CHECK(udFile_Write(pMemo, pString, strLen));
  UD_ERROR_CHECK(udFile_Write(pMemo, "\x1A\x1A", sizeof(char) * 2));

  strLen = blockSize - (strLen % blockSize);

  while (strLen > 0)
  {
    UD_ERROR_CHECK(udFile_Write(pMemo, spaceBuffer, udMin(strLen, (uint32_t)sizeof(spaceBuffer))));
    strLen -= sizeof(spaceBuffer);
  }

  result = udR_Success;

epilogue:

  return result;
}

udResult vcDBF_WriteRecord(vcDBF *pDBF, udFile *pFile, vcDBF_Record *pRecord, udFile *pMemo /*= nullptr*/)
{
  udResult result = udR_Failure_;

  for (int i = 0; i < pDBF->fieldCount; ++i)
  {
    switch(pDBF->pFields[i].mappedType)
    {
    case vcDBFDMT_String: // String types shouldn't be space padded, they're padded with nulls at the end
      vcDBF_WriteString(pFile, pRecord->pFields[i].pString, pDBF->pFields[i].fieldLen);
      break;

    case vcDBFDMT_Float:
      vcDBF_WriteDouble(pFile, pRecord->pFields[i].floaty, pDBF->pFields[i].fieldLen);
      break;

    case vcDBFDMT_Logical:
      vcDBF_WriteBool(pFile, pRecord->pFields[i].logical, pDBF->pFields[i].fieldLen);
      break;

    case vcDBFDMT_Integer:
      vcDBF_WriteInteger(pFile, pRecord->pFields[i].integer, pDBF->pFields[i].fieldLen);
      break;

    case vcDBFDMT_Memo:
      UD_ERROR_IF(!pDBF->memo, udR_Failure_);

      if (pDBF->pFields[i].fieldLen == 4) // write memo num
        vcDBF_WriteInteger(pFile, pRecord->pFields[i].integer, pDBF->pFields[i].fieldLen);
      else
        vcDBF_WriteString(pFile, pRecord->pFields[i].pString, pDBF->pFields[i].fieldLen);

      UD_ERROR_CHECK(vcDBF_WriteMemoBlock(pMemo, pRecord->pFields[i].pString, pDBF->memoData.memoBlockSize));
      break;

    case vcDBFDMT_Invalid:
      break;
    }
  }

  result = udR_Success;

epilogue:

  return result;
}

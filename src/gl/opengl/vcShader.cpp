#include "gl/vcShader.h"
#include "vcOpenGL.h"
#include "udPlatform/udPlatformUtil.h"

GLint vcBuildShader(GLenum type, const GLchar *shaderCode)
{
  GLint compiled;
  GLint shaderCodeLen = (GLint)strlen(shaderCode);
  GLint shaderObject = glCreateShader(type);
  glShaderSource(shaderObject, 1, &shaderCode, &shaderCodeLen);
  glCompileShader(shaderObject);
  glGetShaderiv(shaderObject, GL_COMPILE_STATUS, &compiled);

  if (!compiled)
  {
    GLint blen = 1024;
    GLsizei slen = 0;
    glGetShaderiv(shaderObject, GL_INFO_LOG_LENGTH, &blen);
    if (blen > 1)
    {
      GLchar* compiler_log = (GLchar*)udAlloc(blen);
      glGetShaderInfoLog(shaderObject, blen, &slen, compiler_log);
      udDebugPrintf("%s", compiler_log);
      udFree(compiler_log);
    }
    return -1;
  }

  return shaderObject;
}

GLint vcBuildProgram(GLint vertexShader, GLint fragmentShader)
{
  GLint programObject = glCreateProgram();
  if (vertexShader != -1)
    glAttachShader(programObject, vertexShader);
  if (fragmentShader != -1)
    glAttachShader(programObject, fragmentShader);

  glLinkProgram(programObject);
  GLint linked;
  glGetProgramiv(programObject, GL_LINK_STATUS, &linked);
  if (!linked)
  {
    GLint blen = 1024;
    GLsizei slen = 0;
    glGetProgramiv(programObject, GL_INFO_LOG_LENGTH, &blen);
    if (blen > 1)
    {
      GLchar* linker_log = (GLchar*)udAlloc(blen);
      glGetProgramInfoLog(programObject, blen, &slen, linker_log);
      udDebugPrintf("%s", linker_log);
      udFree(linker_log);
    }
    return -1;
  }

  glDeleteShader(vertexShader);
  glDeleteShader(fragmentShader);

  return programObject;
}

bool vcShader_CreateFromText(vcShader **ppShader, const char *pVertexShader, const char *pFragmentShader, const vcShaderVertexInputTypes * /*pInputTypes*/, uint32_t /*totalInputs*/)
{
  if (ppShader == nullptr || pVertexShader == nullptr || pFragmentShader == nullptr)
    return false;

  vcShader *pShader = udAllocType(vcShader, 1, udAF_Zero);

  pShader->programID = vcBuildProgram(vcBuildShader(GL_VERTEX_SHADER, pVertexShader), vcBuildShader(GL_FRAGMENT_SHADER, pFragmentShader));

  if (pShader->programID == GL_INVALID_INDEX)
    udFree(pShader);

  *ppShader = pShader;

  return (pShader != nullptr);
}

void vcShader_DestroyShader(vcShader **ppShader)
{
  if (ppShader == nullptr || *ppShader == nullptr)
    return;

  glDeleteProgram((*ppShader)->programID);
  udFree(*ppShader);
}

bool vcShader_Bind(vcShader *pShader)
{
  if (pShader != nullptr && pShader->programID == GL_INVALID_INDEX)
    return false;

  if (pShader == nullptr)
    glUseProgram(0);
  else
    glUseProgram(pShader->programID);

  VERIFY_GL();

  return true;
}

bool vcShader_BindTexture(vcShader *pShader, vcTexture *pTexture, uint16_t samplerIndex, vcShaderSampler *pSampler/* = nullptr*/)
{
  if (pTexture == nullptr || pTexture->id == GL_INVALID_INDEX)
    return false;

  udUnused(pShader);

  glActiveTexture(GL_TEXTURE0 + samplerIndex);
  VERIFY_GL();

  if (pTexture->format == vcTextureFormat_Cubemap)
    glBindTexture(GL_TEXTURE_CUBE_MAP, pTexture->id);
  else
    glBindTexture(GL_TEXTURE_2D, pTexture->id);
  VERIFY_GL();

  if (pSampler != nullptr)
    glUniform1i(pSampler->id, samplerIndex);

  VERIFY_GL();

  return true;
}

bool vcShader_GetConstantBuffer(vcShaderConstantBuffer **ppBuffer, vcShader *pShader, const char *pBufferName, const size_t bufferSize)
{
  if (ppBuffer == nullptr || pShader == nullptr || pBufferName == nullptr || bufferSize == 0)
    return false;

  *ppBuffer = nullptr;

  for (int i = 0; i < pShader->numBufferObjects; ++i)
  {
    if (udStrEqual(pShader->bufferObjects[i].name, pBufferName))
    {
      *ppBuffer = &pShader->bufferObjects[i];
      break;
    }
  }

  if (*ppBuffer == nullptr)
  {
    uint32_t blockIndex = glGetUniformBlockIndex(pShader->programID, pBufferName);

    if (blockIndex != GL_INVALID_INDEX)
    {
      glUniformBlockBinding(pShader->programID, blockIndex, pShader->numBufferObjects);
      glBindBufferBase(GL_UNIFORM_BUFFER, pShader->numBufferObjects, pShader->bufferObjects[pShader->numBufferObjects].id);

      glGenBuffers(1, &pShader->bufferObjects[pShader->numBufferObjects].id);
      glBindBuffer(GL_UNIFORM_BUFFER, pShader->bufferObjects[pShader->numBufferObjects].id);
      glBufferData(GL_UNIFORM_BUFFER, bufferSize, NULL, GL_DYNAMIC_DRAW);
      glBindBuffer(GL_UNIFORM_BUFFER, 0);

      udStrcpy(pShader->bufferObjects[pShader->numBufferObjects].name, 32, pBufferName);
      pShader->bufferObjects[pShader->numBufferObjects].bindPoint = pShader->numBufferObjects;
      *ppBuffer = &pShader->bufferObjects[pShader->numBufferObjects];
      ++pShader->numBufferObjects;
    }
  }

  return ((*ppBuffer) != nullptr);
}

bool vcShader_BindConstantBuffer(vcShader *pShader, vcShaderConstantBuffer *pBuffer, const void *pData, const size_t bufferSize)
{
  if (pShader == nullptr || pBuffer == nullptr || pData == nullptr || bufferSize == 0)
    return false;

  //glBindBuffer(GL_UNIFORM_BUFFER, pBuffer->id);
  glBindBufferBase(GL_UNIFORM_BUFFER, pBuffer->bindPoint, pBuffer->id);
  glBufferData(GL_UNIFORM_BUFFER, bufferSize, pData, GL_DYNAMIC_DRAW);

  VERIFY_GL();

  return true;
}

bool vcShader_ReleaseConstantBuffer(vcShader * /*pShader*/, vcShaderConstantBuffer * /*pBuffer*/)
{
  //TODO
  return true;
}

bool vcShader_GetSamplerIndex(vcShaderSampler **ppSampler, vcShader *pShader, const char *pSamplerName)
{
  if (ppSampler == nullptr || pShader == nullptr || pSamplerName == nullptr || pShader->programID == GL_INVALID_INDEX)
    return false;

  GLuint uID = glGetUniformLocation(pShader->programID, pSamplerName);

  if (uID == GL_INVALID_INDEX)
    return false;

  vcShaderSampler *pSampler = udAllocType(vcShaderSampler, 1, udAF_Zero);
  pSampler->id = uID;

  *ppSampler = pSampler;

  return true;
}
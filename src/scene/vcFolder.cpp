#include "vcFolder.h"

#include "vcScene.h"
#include "vcState.h"
#include "vcStrings.h"

#include "gl/vcFenceRenderer.h"

#include "imgui.h"
#include "imgui_ex/vcImGuiSimpleWidgets.h"

#include <chrono>

// TODO: Rename vcMain functions
static int64_t vcMain_GetCurrentTime(int fractionSec = 1) // This gives 1/fractionSec factions since epoch, 5=200ms, 10=100ms etc.
{
  return std::chrono::system_clock::now().time_since_epoch().count() * fractionSec / std::chrono::system_clock::period::den;
}

static void vcMain_ShowLoadStatusIndicator(vcSceneLoadStatus loadStatus, bool sameLine = true)
{
  const char *loadingChars[] = { "\xE2\x96\xB2", "\xE2\x96\xB6", "\xE2\x96\xBC", "\xE2\x97\x80" };
  int64_t currentLoadingChar = vcMain_GetCurrentTime(10);

  // Load Status (if any)
  if (loadStatus == vcSLS_Pending)
  {
    ImGui::TextColored(ImVec4(1.f, 1.f, 0.f, 1.f), "\xE2\x9A\xA0"); // Yellow Exclamation in Triangle
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", vcString::Get("Pending"));

    if (sameLine)
      ImGui::SameLine();
  }
  else if (loadStatus == vcSLS_Loading)
  {
    ImGui::TextColored(ImVec4(1.f, 1.f, 0.f, 1.f), "%s", loadingChars[currentLoadingChar % udLengthOf(loadingChars)]); // Yellow Spinning clock
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", vcString::Get("Loading"));

    if (sameLine)
      ImGui::SameLine();
  }
  else if (loadStatus == vcSLS_Failed || loadStatus == vcSLS_OpenFailure)
  {
    ImGui::TextColored(ImVec4(1.f, 0.f, 0.f, 1.f), "\xE2\x9A\xA0"); // Red Exclamation in Triangle
    if (ImGui::IsItemHovered())
    {
      if (loadStatus == vcSLS_OpenFailure)
        ImGui::SetTooltip("%s", vcString::Get("ModelOpenFailure"));
      else
        ImGui::SetTooltip("%s", vcString::Get("ModelLoadFailure"));
    }

    if (sameLine)
      ImGui::SameLine();
  }
}

void vcFolder::AddToScene(vcState *pProgramState, vcRenderData *pRenderData)
{
  if (!visible)
    return;

  for (size_t i = 0; i < children.size(); ++i)
    children[i]->AddToScene(pProgramState, pRenderData);
}

void vcFolder::ApplyDelta(vcState * /*pProgramState*/)
{
  // Maybe recurse children and call ApplyDelta?
}

void vcFolder::HandleImGui(vcState *pProgramState, size_t *pItemID)
{
  size_t i;
  for (i = 0; i < children.size(); ++i)
  {
    ++(*pItemID);

    // This block is also after the loop
    if (this == pProgramState->sceneExplorer.insertItem.pParent && i == pProgramState->sceneExplorer.insertItem.index)
    {
      ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1.f, 1.f, 0.f, 1.f)); // RGBA
      ImVec2 pos = ImGui::GetCursorPos();
      ImGui::Separator();
      ImGui::SetCursorPos(pos);
      ImGui::PopStyleColor();
    }

    // Can only edit the name while the item is still selected
    children[i]->editName = children[i]->editName && children[i]->selected;

    // Visibility
    ImGui::Checkbox(udTempStr("###SXIVisible%zu", *pItemID), &children[i]->visible);
    ImGui::SameLine();

    vcMain_ShowLoadStatusIndicator((vcSceneLoadStatus)children[i]->loadStatus);

    // The actual model
    ImGui::SetNextTreeNodeOpen(children[i]->expanded, ImGuiCond_Always);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow;
    if (children[i]->selected)
      flags |= ImGuiTreeNodeFlags_Selected;

    if (children[i]->editName)
    {
      children[i]->expanded = ImGui::TreeNodeEx(udTempStr("###SXIName%zu", *pItemID), flags);
      ImGui::SameLine();
      if (vcIGSW_InputTextWithResize(udTempStr("###FolderName%zu", *pItemID), &children[i]->pName, &children[i]->nameBufferLength, ImGuiInputTextFlags_EnterReturnsTrue))
        children[i]->editName = false;
    }
    else
    {
      children[i]->expanded = ImGui::TreeNodeEx(udTempStr("%s###SXIName%zu", children[i]->pName, *pItemID), flags);
      children[i]->editName = ImGui::IsItemClicked() && ImGui::IsMouseDoubleClicked(0);
    }

    if ((ImGui::IsMouseReleased(0) && ImGui::IsItemHovered() && !ImGui::IsItemActive()) || (!children[i]->selected && ImGui::IsItemActive()))
    {
      if (!ImGui::GetIO().KeyCtrl)
        vcScene_ClearSelection(pProgramState);

      if (children[i]->selected)
      {
        vcScene_UnselectItem(pProgramState, this, i);
        pProgramState->sceneExplorer.clickedItem = { nullptr, SIZE_MAX };
      }
      else
      {
        vcScene_SelectItem(pProgramState, this, i);
        pProgramState->sceneExplorer.clickedItem = { this, i };
      }
    }

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && !ImGui::IsItemHovered() && !ImGui::IsItemActive())
    {
      ImVec2 minPos = ImGui::GetItemRectMin();
      ImVec2 maxPos = ImGui::GetItemRectMax();
      ImVec2 mousePos = ImGui::GetMousePos();

      if (children[i]->type == vcSOT_Folder && udAbs(mousePos.y - minPos.y) >= udAbs(mousePos.y - maxPos.y))
        pProgramState->sceneExplorer.insertItem = { (vcFolder*)children[i], ((vcFolder*)children[i])->children.size() };
      else if (udAbs(mousePos.y - minPos.y) < udAbs(mousePos.y - maxPos.y))
        pProgramState->sceneExplorer.insertItem = { this, i };
      else
        pProgramState->sceneExplorer.insertItem = { this, i + 1 };
    }

    if (ImGui::BeginPopupContextItem(udTempStr("ModelContextMenu_%zu", *pItemID)))
    {
      if (children[i]->pZone != nullptr && ImGui::Selectable(vcString::Get("UseProjection")))
      {
        if (vcGIS_ChangeSpace(&pProgramState->gis, children[i]->pZone->srid, &pProgramState->pCamera->position))
          vcScene_UpdateItemToCurrentProjection(pProgramState, nullptr); // Update all models to new zone
      }

      if (ImGui::Selectable(vcString::Get("MoveTo")))
      {
        udDouble3 localSpaceCenter = vcScene_GetItemWorldSpacePivotPoint(children[i]);

        // Transform the camera position. Don't do the entire matrix as it may lead to inaccuracy/de-normalised camera
        if (pProgramState->gis.isProjected && children[i]->pZone != nullptr && children[i]->pZone->srid != pProgramState->gis.SRID)
          localSpaceCenter = udGeoZone_TransformPoint(localSpaceCenter, *children[i]->pZone, pProgramState->gis.zone);

        pProgramState->cameraInput.inputState = vcCIS_MovingToPoint;
        pProgramState->cameraInput.startPosition = pProgramState->pCamera->position;
        pProgramState->cameraInput.startAngle = udDoubleQuat::create(pProgramState->pCamera->eulerRotation);
        pProgramState->cameraInput.worldAnchorPoint = localSpaceCenter;
        pProgramState->cameraInput.progress = 0.0;
      }

      ImGui::EndPopup();
    }

    if (ImGui::IsMouseDoubleClicked(0) && ImGui::IsItemHovered())
      vcScene_UseProjectFromItem(pProgramState, children[i]);

    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", children[i]->pName);

    // Show additional settings from ImGui
    if (children[i]->expanded)
    {
      ImGui::Indent();
      ImGui::PushID(udTempStr("SXIExpanded%zu", *pItemID));

      children[i]->HandleImGui(pProgramState, pItemID);

      ImGui::PopID();
      ImGui::Unindent();
      ImGui::TreePop();
    }
  }

  // This block is also in the loop above
  if (this == pProgramState->sceneExplorer.insertItem.pParent && i == pProgramState->sceneExplorer.insertItem.index)
  {
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(1.f, 1.f, 0.f, 1.f)); // RGBA
    ImGui::Separator();
    ImGui::PopStyleColor();
  }
}

void vcFolder::Cleanup(vcState *pProgramState)
{
  udFree(pName);

  while (children.size() > 0)
    vcScene_RemoveItem(pProgramState, this, 0);

  this->vcFolder::~vcFolder();
}

void vcFolder_AddToList(vcState *pProgramState, const char *pName)
{
  vcFolder *pFolder =  udAllocType(vcFolder, 1, udAF_Zero);
  pFolder = new (pFolder) vcFolder();
  pFolder->visible = true;

  pFolder->pName = udStrdup(pName);
  pFolder->type = vcSOT_Folder;

  pFolder->children.reserve(64);

  udStrcpy(pFolder->typeStr, sizeof(pFolder->typeStr), "Folder");
  pFolder->loadStatus = vcSLS_Loaded;

  if (pName == nullptr)
    pProgramState->sceneExplorer.pItems = pFolder;
  else
    pFolder->AddItem(pProgramState);
}

#include "SDL2/SDL.h"
#include "udPlatform/udPlatform.h"
#include "udPlatform/udMath.h"
#include "vcCamera.h"

#ifndef vcSettings_h__
#define vcSettings_h__

enum vcMapTileBlendMode
{
  vcMTBM_Hybrid,
  vcMTBM_Overlay,
  vcMTBM_Underlay,

  vcMTBM_Count
};

enum vcDocks
{
  vcDocks_Scene,
  vcDocks_Settings,
  vcDocks_SceneExplorer,
  vcDocks_Convert,

  vcDocks_Count
};

enum vcVisualizatationMode
{
  vcVM_Colour,
  vcVM_Intensity,
  vcVM_Classification,

  vcVM_Count
};

enum vcAnchorStyle
{
  vcAS_None,
  vcAS_Orbit,
  vcAS_Compass,

  vcAS_Count
};

enum
{
  vcMaxPathLength = 512,
};

struct vcSettings
{
  bool noLocalStorage; //If set to true; cannot save or load from local storage
  const char *pSaveFilePath;

  struct
  {
    int styleIndex;
    bool showDiagnosticInfo;
    bool showAdvancedGIS;

    vcAnchorStyle mouseAnchor;
    bool showCompass;

    bool limitFPSInBackground;

    int pointMode;
  } presentation;

  struct
  {
    int xpos;
    int ypos;
    int width;
    int height;
    bool maximized;
    bool fullscreen;
    bool touchscreenFriendly;

    bool windowsOpen[vcDocks_Count];
  } window;

  struct
  {
    bool rememberServer;
    char serverURL[vcMaxPathLength];

    bool rememberUsername;
    char username[vcMaxPathLength];

    char proxy[vcMaxPathLength];
    bool ignoreCertificateVerification;
  } loginInfo;

  struct
  {
    vcVisualizatationMode mode;
    int minIntensity;
    int maxIntensity;
  } visualization;

  struct
  {
    struct
    {
      bool enable;
      int width;
      float threshold;
      udFloat4 colour;
    } edgeOutlines;

    struct
    {
      bool enable;
      udFloat4 minColour;
      udFloat4 maxColour;
      float startHeight;
      float endHeight;
    } colourByHeight;

    struct
    {
      bool enable;
      udFloat4 colour;
      float startDepth;
      float endDepth;
    } colourByDepth;

    struct
    {
      bool enable;
      udFloat4 colour;
      float distances;
      float bandHeight;
    } contours;
  } postVisualization;

  vcCameraSettings camera;

  struct
  {
    bool mapEnabled;

    float mapHeight;
    char tileServerAddress[vcMaxPathLength];
    char tileServerExtension[4];

    vcMapTileBlendMode blendMode;
    float transparency;

    bool mouseInteracts;
  } maptiles;
};

// Settings Limits (vcSL prefix)
const float vcSL_CameraNearPlaneMin = 0.01f;
const float vcSL_CameraNearPlaneMax = 1000.f;

const float vcSL_CameraFarPlaneMin = vcSL_CameraNearPlaneMax;
const float vcSL_CameraFarPlaneMax = 1000000.f;

const float vcSL_CameraNearFarPlaneRatioMax = 20000.f;

const float vcSL_CameraMinMoveSpeed = 0.5f;
const float vcSL_CameraMaxMoveSpeed = 10000.f;

const float vcSL_CameraFieldOfViewMin = 5;
const float vcSL_CameraFieldOfViewMax = 100;

const float vcSL_OSCPixelRatio = 100.f;

// Settings Functions
bool vcSettings_Load(vcSettings *pSettings, bool forceReset = false);
bool vcSettings_Save(vcSettings *pSettings);

// Uses udTempStr internally.
const char *vcSettings_GetAssetPath(const char *pFilename);

// Provides a handler for "asset://" files
udResult vcSettings_RegisterAssetFileHandler();

#endif // !vcSettings_h__

/*
 * ReShadeUI.fxh  -  self-authored compatibility stub for the ReShade UI helper
 * macros. The PSOBB shaders in this repo use ReShade's native uniform
 * annotations directly (ui_type / ui_min / ui_max / ui_items / ui_label), so
 * this header only provides the optional __UNIFORM_* convenience macros some
 * third-party shaders expect. Mirrors the public-domain (CC0-1.0) upstream
 * interface; independent re-implementation.
 */

#pragma once

#define __UNIFORM_INPUT_INT1     ui_type = "slider";
#define __UNIFORM_INPUT_FLOAT1   ui_type = "slider";

#define __UNIFORM_SLIDER_INT1    ui_type = "slider";
#define __UNIFORM_SLIDER_FLOAT1  ui_type = "slider";
#define __UNIFORM_SLIDER_FLOAT2  ui_type = "slider";
#define __UNIFORM_SLIDER_FLOAT3  ui_type = "slider";

#define __UNIFORM_DRAG_INT1      ui_type = "drag";
#define __UNIFORM_DRAG_FLOAT1    ui_type = "drag";

#define __UNIFORM_COMBO_INT1     ui_type = "combo";
#define __UNIFORM_RADIO_INT1     ui_type = "radio";
#define __UNIFORM_LIST_INT1      ui_type = "list";

#define __UNIFORM_COLOR_FLOAT3   ui_type = "color";
#define __UNIFORM_COLOR_FLOAT4   ui_type = "color";

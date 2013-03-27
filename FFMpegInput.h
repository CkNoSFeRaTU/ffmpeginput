#pragma once

#include "OBSApi.h"

#include <dshow.h>
#include <Amaudio.h>
#include <Dvdmedia.h>

#include "FFMpegSource.h"
#include "resource.h"

//-----------------------------------------------------------

extern HINSTANCE hinstMain;

extern LocaleStringLookup *pluginLocale;
#define PluginStr(text) pluginLocale->LookupString(TEXT2(text))

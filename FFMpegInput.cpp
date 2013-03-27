#include "FFMpegInput.h"

extern "C" __declspec(dllexport) bool LoadPlugin();
extern "C" __declspec(dllexport) void UnloadPlugin();
extern "C" __declspec(dllexport) CTSTR GetPluginName();
extern "C" __declspec(dllexport) CTSTR GetPluginDescription();

LocaleStringLookup *pluginLocale = NULL;
HINSTANCE hinstMain = NULL;


#define DSHOW_CLASSNAME TEXT("FFMpegSource")

struct ConfigFFMpegSourceInfo
{
    XElement *data;
    UINT cx, cy;
};

INT_PTR CALLBACK ConfigureDialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch(message)
    {
        case WM_INITDIALOG:
            {
				ConfigFFMpegSourceInfo *configInfo = (ConfigFFMpegSourceInfo*)lParam;
                SetWindowLongPtr(hwnd, DWLP_USER, (LONG_PTR)configInfo);
                LocalizeWindow(hwnd, pluginLocale);

                //--------------------------

                HWND hwndTemp = GetDlgItem(hwnd, IDC_FILES);

                StringList filesList;
                configInfo->data->GetStringList(TEXT("files"), filesList);
                for(UINT i=0; i<filesList.Num(); i++)
                {
                    CTSTR lpFile = filesList[i];

//                    if(OSFileExists(lpFile))
                        SendMessage(hwndTemp, LB_ADDSTRING, 0, (LPARAM)lpFile);
                }

                //--------------------------

                EnableWindow(GetDlgItem(hwnd, IDC_REMOVE), FALSE);
                EnableWindow(GetDlgItem(hwnd, IDC_MOVEUPWARD), FALSE);
                EnableWindow(GetDlgItem(hwnd, IDC_MOVEDOWNWARD), FALSE);

                return TRUE;
            }

        case WM_COMMAND:
            switch(LOWORD(wParam))
            {
                case IDC_ADDURL:
                    {
                        TSTR lpFile = (TSTR)Allocate(255*sizeof(TCHAR));
                        zero(lpFile, 255*sizeof(TCHAR));

-						GetDlgItemText(hwnd, IDC_EDITURL, (LPTSTR)lpFile, 255);

						String strPath = lpFile;

						UINT idExisting = (UINT)SendMessage(GetDlgItem(hwnd, IDC_FILES), LB_FINDSTRINGEXACT, -1, (LPARAM)strPath.Array());
                        if(idExisting == LB_ERR)
                            SendMessage(GetDlgItem(hwnd, IDC_FILES), LB_ADDSTRING, 0, (LPARAM)strPath.Array());

                        Free(lpFile);
					}
					break;

                case IDC_ADDFILE:
                    {
                        TSTR lpFile = (TSTR)Allocate(32*1024*sizeof(TCHAR));
                        zero(lpFile, 32*1024*sizeof(TCHAR));

                        OPENFILENAME ofn;
                        zero(&ofn, sizeof(ofn));
                        ofn.lStructSize = sizeof(ofn);
                        ofn.lpstrFile = lpFile;
                        ofn.hwndOwner = hwnd;
                        ofn.nMaxFile = 32*1024*sizeof(TCHAR);
                        ofn.lpstrFilter = TEXT("All Formats (*.avi;*.mkv;*.mp4;*.ogg;*.flv)\0*.avi;*.mkv;*.mp4;*.ogg;*.flv\0");
                        ofn.nFilterIndex = 1;
                        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER;

                        TCHAR curDirectory[MAX_PATH+1];
                        GetCurrentDirectory(MAX_PATH, curDirectory);

                        BOOL bOpenFile = GetOpenFileName(&ofn);

                        TCHAR newDirectory[MAX_PATH+1];
                        GetCurrentDirectory(MAX_PATH, newDirectory);

                        SetCurrentDirectory(curDirectory);

                        if(bOpenFile)
                        {
                            TSTR lpCurFile = lpFile+ofn.nFileOffset;

                            while(lpCurFile && *lpCurFile)
                            {
                                String strPath;
                                strPath << newDirectory << TEXT("\\") << lpCurFile;

                                UINT idExisting = (UINT)SendMessage(GetDlgItem(hwnd, IDC_FILES), LB_FINDSTRINGEXACT, -1, (LPARAM)strPath.Array());
                                if(idExisting == LB_ERR)
                                    SendMessage(GetDlgItem(hwnd, IDC_FILES), LB_ADDSTRING, 0, (LPARAM)strPath.Array());

                                lpCurFile += slen(lpCurFile)+1;
                            }
                        }

                        Free(lpFile);

                        break;
					}

                case IDC_FILES:
                    if(HIWORD(wParam) == LBN_SELCHANGE)
                    {
                        EnableWindow(GetDlgItem(hwnd, IDC_REMOVE), TRUE);
                        EnableWindow(GetDlgItem(hwnd, IDC_MOVEUPWARD), TRUE);
                        EnableWindow(GetDlgItem(hwnd, IDC_MOVEDOWNWARD), TRUE);
                    }
                    break;

                case IDC_REMOVE:
                    {
                        UINT curSel = (UINT)SendMessage(GetDlgItem(hwnd, IDC_FILES), LB_GETCURSEL, 0, 0);
                        if(curSel != LB_ERR)
                        {
                            SendMessage(GetDlgItem(hwnd, IDC_FILES), LB_DELETESTRING, curSel, 0);
                            EnableWindow(GetDlgItem(hwnd, IDC_REMOVE), FALSE);
                            EnableWindow(GetDlgItem(hwnd, IDC_MOVEUPWARD), FALSE);
                            EnableWindow(GetDlgItem(hwnd, IDC_MOVEDOWNWARD), FALSE);
                        }
                    }
                    break;

                case IDC_MOVEUPWARD:
                    {
                        HWND hwndFiles = GetDlgItem(hwnd, IDC_FILES);
                        UINT curSel = (UINT)SendMessage(hwndFiles, LB_GETCURSEL, 0, 0);
                        if(curSel != LB_ERR)
                        {
                            if(curSel > 0)
                            {
                                String strText = GetLBText(hwndFiles, curSel);

                                SendMessage(hwndFiles, LB_DELETESTRING, curSel, 0);
                                SendMessage(hwndFiles, LB_INSERTSTRING, --curSel, (LPARAM)strText.Array());
                                PostMessage(hwndFiles, LB_SETCURSEL, curSel, 0);
                            }
                        }
                    }
                    break;

                case IDC_MOVEDOWNWARD:
                    {
                        HWND hwndFiles = GetDlgItem(hwnd, IDC_FILES);

                        UINT numFiles = (UINT)SendMessage(hwndFiles, LB_GETCOUNT, 0, 0);
                        UINT curSel = (UINT)SendMessage(hwndFiles, LB_GETCURSEL, 0, 0);
                        if(curSel != LB_ERR)
                        {
                            if(curSel < (numFiles-1))
                            {
                                String strText = GetLBText(hwndFiles, curSel);

                                SendMessage(hwndFiles, LB_DELETESTRING, curSel, 0);
                                SendMessage(hwndFiles, LB_INSERTSTRING, ++curSel, (LPARAM)strText.Array());
                                PostMessage(hwndFiles, LB_SETCURSEL, curSel, 0);
                            }
                        }
                    }
                    break;

                case IDOK:
                    {
						HWND hwndFiles = GetDlgItem(hwnd, IDC_FILES);

                        UINT numFiles = (UINT)SendMessage(hwndFiles, LB_GETCOUNT, 0, 0);
                        if(!numFiles)
                        {
                            MessageBox(hwnd, PluginStr("Sources.FFMpegSource.Empty"), NULL, 0);
                            break;
                        }

                        //---------------------------

                        StringList filesList;
                        for(UINT i=0; i<numFiles; i++)
                            filesList << GetLBText(hwndFiles, i);

                        ConfigFFMpegSourceInfo *configInfo = (ConfigFFMpegSourceInfo*)GetWindowLongPtr(hwnd, DWLP_USER);

                        configInfo->data->SetStringList(TEXT("files"), filesList);

						}

                case IDCANCEL:
                    EndDialog(hwnd, LOWORD(wParam));
                    break;
            }
            break;
    }

    return 0;
}

bool STDCALL ConfigureFFMpegInputSource(XElement *element, bool bCreating)
{
    if(!element)
    {
        AppWarning(TEXT("ConfigureFFMpegInputSource: NULL element"));
        return false;
    }

    XElement *data = element->GetElement(TEXT("data"));
    if(!data)
        data = element->CreateElement(TEXT("data"));

    ConfigFFMpegSourceInfo configInfo;
    configInfo.data = data;

    if(DialogBoxParam(hinstMain, MAKEINTRESOURCE(IDD_CONFIGUREFFMPEGSOURCE), API->GetMainWindow(), ConfigureDialogProc, (LPARAM)&configInfo) == IDOK)
    {
		element->SetInt(TEXT("cx"), 640);
		element->SetInt(TEXT("cy"), 480);
	}

    return true;

}

ImageSource* STDCALL CreateFFMpegInputSource(XElement *data)
{
	FFMpegSource *source = new FFMpegSource;
    if(!source->Init(data))
    {
        delete source;
        return NULL;
    }

    return source;
}


bool LoadPlugin()
{
    InitColorControl(hinstMain);

    pluginLocale = new LocaleStringLookup;

    if(!pluginLocale->LoadStringFile(TEXT("plugins/FFMpegInput/locale/en.txt")))
        AppWarning(TEXT("Could not open locale string file '%s'"), TEXT("plugins/FFMpegInput/locale/en.txt"));

    if(scmpi(API->GetLanguage(), TEXT("en")) != 0)
    {
        String pluginStringFile;
        pluginStringFile << TEXT("plugins/FFMpegInput/locale/") << API->GetLanguage() << TEXT(".txt");
        if(!pluginLocale->LoadStringFile(pluginStringFile))
            AppWarning(TEXT("Could not open locale string file '%s'"), pluginStringFile.Array());
    }

    API->RegisterImageSourceClass(DSHOW_CLASSNAME, PluginStr("ClassName"), (OBSCREATEPROC)CreateFFMpegInputSource, (OBSCONFIGPROC)ConfigureFFMpegInputSource);

    return true;
}

void UnloadPlugin()
{
    delete pluginLocale;
}

CTSTR GetPluginName()
{
    return PluginStr("Plugin.Name");
}

CTSTR GetPluginDescription()
{
    return PluginStr("Plugin.Description");
}


BOOL CALLBACK DllMain(HINSTANCE hInst, DWORD dwReason, LPVOID lpBla)
{
    if(dwReason == DLL_PROCESS_ATTACH)
        hinstMain = hInst;

    return TRUE;
}


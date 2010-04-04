/*  Pcsx - Pc Psx Emulator
 *  Copyright (C) 1999-2003  Pcsx Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>
#include <string>

#include <fcntl.h>
#include <io.h>

#include "resource.h"
#include "AboutDlg.h"

#include "../version.h"
#include "PsxCommon.h"
#include "plugin.h"
#include "Debug.h"
#include "Win32.h"
#include "../cheat.h"
#include "../movie.h"
#include "moviewin.h"
#include "ram_search.h"
#include "ramwatch.h"

extern HWND LuaConsoleHWnd;
extern INT_PTR CALLBACK DlgLuaScriptDialog(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);

// global variables
AppData gApp;
HANDLE hConsole;
long LoadCdBios;
char cfgfile[256];
int Running;
char PcsxDir[256];

extern bool OpenPlugins(HWND hWnd);

int iSaveStateTo;
int iLoadStateFrom;
int iCallW32Gui;
char szCurrentPath[256];
char szMovieToLoad[256];
char szWatchToLoad[256];


uint32 mousex,mousey;

HWND memPokeHWND = NULL;
char bufPokeAddress[7];
char bufPokeNewval[7];

#ifdef __MINGW32__
#ifndef LVM_GETSELECTIONMARK
#define LVM_GETSELECTIONMARK (LVM_FIRST+66)
#endif
#ifndef ListView_GetSelectionMark
#define ListView_GetSelectionMark(w) (INT)SNDMSG((w),LVM_GETSELECTIONMARK,0,0)
#endif
#endif

#include "maphkeys.h"
int iTurboMode;

int AccBreak=0;
int ConfPlug=0;
int StatesC=0;
int NeedReset=1;
int cdOpenCase=0;
int cheatsEnabled=0;
int CancelQuit=0;
int UseGui = 1;

#define MAXFILENAME 256

void strcatz(char *dst, char *src) {
	int len = strlen(dst) + 1;
	strcpy(dst + len, src);
}

void GetCurrentPath()
{
	char szPath[256];
	char szDrive[256];
	char szDirectory[256];
	char szFilename[256];
	char szExt[256];
	szDrive[0] = '\0';
	szDirectory[0] = '\0';
	szFilename[0] = '\0';
	szExt[0] = '\0';
	GetModuleFileName(NULL, szPath, 256);
	_splitpath(szPath, szDrive, szDirectory, szFilename, szExt);

	sprintf(szCurrentPath,"%s%s" + '\0',szDrive, szDirectory);
}

PCHAR*    CommandLineToArgvA( PCHAR CmdLine, int* _argc)
{
	PCHAR* argv;
	PCHAR  _argv;
	ULONG  len;
	ULONG  argc;
	CHAR   a;
	ULONG  i, j;

	BOOLEAN  in_QM;
	BOOLEAN  in_TEXT;
	BOOLEAN  in_SPACE;

	len = strlen(CmdLine);
	i = ((len+2)/2)*sizeof(PVOID) + sizeof(PVOID);

	argv = (PCHAR*)GlobalAlloc(GMEM_FIXED, i + (len+2)*sizeof(CHAR));

	_argv = (PCHAR)(((PUCHAR)argv)+i);

	argc = 0;
	argv[argc] = _argv;
	in_QM = FALSE;
	in_TEXT = FALSE;
	in_SPACE = TRUE;
	i = 0;
	j = 0;

	while(( a = CmdLine[i] )) {
		if(in_QM) {
			if(a == '\"') {
				in_QM = FALSE;
			} else {
				_argv[j] = a;
				j++;
			}
		} else {
			switch(a) {
			case '\"':
				in_QM = TRUE;
				in_TEXT = TRUE;
				if(in_SPACE) {
					argv[argc] = _argv+j;
					argc++;
				}
				in_SPACE = FALSE;
				break;
			case ' ':
			case '\t':
			case '\n':
			case '\r':
				if(in_TEXT) {
					_argv[j] = '\0';
					j++;
				}
				in_TEXT = FALSE;
				in_SPACE = TRUE;
				break;
			default:
				in_TEXT = TRUE;
				if(in_SPACE) {
					argv[argc] = _argv+j;
					argc++;
				}
				_argv[j] = a;
				j++;
				in_SPACE = FALSE;
				break;
			}
		}
		i++;
	}
	_argv[j] = '\0';
	argv[argc] = NULL;

	(*_argc) = argc;
	return argv;
}

//void OpenConsole() 
//{
//	COORD csize;
//	CONSOLE_SCREEN_BUFFER_INFO csbiInfo; 
//	SMALL_RECT srect;
//	char buf[256];
//
//	//dont do anything if we're already attached
//	if (hConsole) return;
//
//	//attach to an existing console (if we can; this is circuitous because AttachConsole wasnt added until XP)
//	//remember to abstract this late bound function notion if we end up having to do this anywhere else
//	bool attached = false;
//	HMODULE lib = LoadLibrary("kernel32.dll");
//	if(lib)
//	{
//		typedef BOOL (WINAPI *_TAttachConsole)(DWORD dwProcessId);
//		_TAttachConsole _AttachConsole  = (_TAttachConsole)GetProcAddress(lib,"AttachConsole");
//		if(_AttachConsole)
//		{
//			if(_AttachConsole(-1))
//				attached = true;
//		}
//		FreeLibrary(lib);
//	}
//
//	//if we failed to attach, then alloc a new console
//	if(!attached)
//	{
//		AllocConsole();
//	}
//
//	hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
//
//	//redirect stdio
//	long lStdHandle = (long)hConsole;
//	int hConHandle = _open_osfhandle(lStdHandle, _O_TEXT);
//	if(hConHandle == -1)
//		return; //this fails from a visual studio command prompt
//	
//	FILE *fp = _fdopen( hConHandle, "w" );
//	*stdout = *fp;
//	//and stderr
//	*stderr = *fp;
//
//	memset(buf,0,256);
//	sprintf(buf,"pcsx-rr OUTPUT");
//	SetConsoleTitle(TEXT(buf));
//	csize.X = 60;
//	csize.Y = 800;
//	SetConsoleScreenBufferSize(GetStdHandle(STD_OUTPUT_HANDLE), csize);
//	GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbiInfo);
//	srect = csbiInfo.srWindow;
//	srect.Right = srect.Left + 99;
//	srect.Bottom = srect.Top + 64;
//	SetConsoleWindowInfo(GetStdHandle(STD_OUTPUT_HANDLE), TRUE, &srect);
//	SetConsoleCP(GetACP());
//	SetConsoleOutputCP(GetACP());
//	if(attached) printf("\n");
//	printf("pcsx-rr\n");
//	printf("- compiled: %s %s\n\n",__DATE__,__TIME__);
//}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
	int runcd=0;
	int loadMovie=0;
	int loadWatch=0;
	int i, argc;
	PCHAR *argv;

	printf ("PCSX\n");
	strcpy(cfgfile, "Software\\PCSX-RR");

	argv = CommandLineToArgvA(GetCommandLine(), &argc);
	if( argc > 1 )
	for( i=1; i < argc; i++ ) {
//		MessageBox( NULL, argv[i], "Argument list", MB_ICONINFORMATION );
		if (!strcmp(argv[i], "-runcd"))
			runcd = 1;
		else if (!strcmp(argv[i], "-runcdbios"))
			runcd = 2;
		else if (!strcmp(argv[i], "-play")) {
			loadMovie = 1;
			sprintf(szMovieToLoad,"%s",argv[++i]);
		}
		else if (!strcmp(argv[i], "-dumpavi")) {
			Movie.startAvi = 1;
			sprintf(Movie.aviFilename,"%s",argv[++i]);
		}
		else if (!strcmp(argv[i], "-dumpwav")) {
			Movie.startWav = 1;
			sprintf(Movie.wavFilename,"%s",argv[++i]);
		}
		else if (!strcmp(argv[i], "-lua")) {
			PCSX_LoadLuaCode(argv[++i]);
		}
		else if (!strcmp(argv[i], "-stopcapture"))
			sscanf (argv[++i],"%lu",&Movie.stopCapture);
		else if (!strcmp(argv[i], "-readonly"))
			Movie.readOnly = 1;
		else if (!strcmp(argv[i], "-memwatch")) {
			loadWatch = 1;
			sprintf(szWatchToLoad,"%s",argv[++i]);
		}
	}

	GetCurrentPath();

	gApp.hInstance = GetModuleHandle(NULL);

	Running=0;

	GetCurrentDirectory(256, PcsxDir);

	memset(&Config, 0, sizeof(PcsxConfig));
	strcpy(Config.Net, "Disabled");
	sprintf(Config.PluginsDir, "%splugins\\", szCurrentPath);
	sprintf(Config.BiosDir, "%sbios\\", szCurrentPath);
	if (LoadConfig() == -1) {
		Config.PsxAuto = 1;
		Config.PauseAfterPlayback = 1;
		strcpy(Config.Bios,       "scph1001.bin");
		strcpy(Config.Gpu,        "gpuTASsoft.dll");
		strcpy(Config.Spu,        "spuTAS.dll");
		strcpy(Config.Cdr,        "cdrTASiso.dll");
		strcpy(Config.Pad1,       "padSeguDPP.dll");
		strcpy(Config.Pad2,       "padSeguDPP.dll");
		SysMessage(_("PCSX-RR needs to be configured."));
		ConfPlug=1;
		ConfigurePlugins(gApp.hWnd);
		DialogBox(gApp.hInstance, MAKEINTRESOURCE(IDD_MCDCONF), gApp.hWnd, (DLGPROC)ConfigureMcdsDlgProc);
		SysMessage(_("PCSX-RR now will quit, restart it."));
		return 0;
	}

	if (SysInit() == -1) return 1;

	CreateMainWindow(SW_SHOW);

	//process some command line options
	if (runcd == 1)
		PostMessage(gApp.hWnd, WM_COMMAND, ID_FILE_RUN_CD, 0);
	else if (runcd == 2)
		PostMessage(gApp.hWnd, WM_COMMAND, ID_FILE_RUNCDBIOS, 0);
	else if (loadMovie)
		WIN32_StartMovieReplay(szMovieToLoad);

	if (loadWatch) {
		RamWatchHWnd = CreateDialog(gApp.hInstance, MAKEINTRESOURCE(IDD_RAMWATCH), NULL, (DLGPROC) RamWatchProc);
		if (!Load_Watches(true, szWatchToLoad)) {
			char errorMessage[1024];
			sprintf(errorMessage, "RAM Watch file \"%s\" doesn't exist.",szWatchToLoad);
			MessageBox(NULL, errorMessage, NULL, MB_ICONERROR);
		}
	}

	RunGui();

	return 0;
}

void RunMessageLoop() {
	MSG msg;

	while(PeekMessage(&msg, NULL, 0, 0, PM_NOREMOVE))
	{
		if(GetMessage(&msg, NULL, 0, 0) > 0)
		{
			if (RamWatchHWnd && IsDialogMessage(RamWatchHWnd, &msg))
			{
				if(msg.message == WM_KEYDOWN) // send keydown messages to the dialog (for accelerators, and also needed for the Alt key to work)
					SendMessage(RamWatchHWnd, msg.message, msg.wParam, msg.lParam);
				continue;
			}
			if (RamSearchHWnd && IsDialogMessage(RamSearchHWnd, &msg))
				continue;

			//if(!TranslateAccelerator(gApp.hWnd,hAccel,&msg))
			{
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
		}
	}
}

void RunGui() {
	for(;;) RunMessageLoop();
}

void RestoreWindow() {
	AccBreak = 1;
	DestroyWindow(gApp.hWnd);
	CreateMainWindow(SW_SHOWNORMAL);
	ShowCursor(TRUE);
	SetCursor(LoadCursor(gApp.hInstance, IDC_ARROW));
	ShowCursor(TRUE);
}

int Slots[5] = { -1, -1, -1, -1, -1 };

void ResetMenuSlots() {
	int i;

	for (i=0; i<5; i++) {
		if (Slots[i] == -1)
			EnableMenuItem(GetSubMenu(gApp.hMenu, 0), ID_FILE_STATES_LOAD_SLOT1+i, MF_GRAYED);
		else 
			EnableMenuItem(GetSubMenu(gApp.hMenu, 0), ID_FILE_STATES_LOAD_SLOT1+i, MF_ENABLED);
	}
}

void UpdateMenuSlots() {
	char str[256];
	int i;

	for (i=0; i<5; i++) {
		sprintf(str, "sstates\\%10.10s.%3.3d", CdromLabel, i);
		Slots[i] = CheckState(str);
	}
}


void CloseConsole() {
	FreeConsole(); hConsole = NULL;
}

void States_Load(int num) {
	char Text[256];
	int ret;

	SetMenu(gApp.hWnd, NULL);
	OpenPlugins(gApp.hWnd);
	SysReset();
	NeedReset = 0;

	sprintf (Text, "sstates\\%10.10s.%3.3d", CdromLabel, num);
	ret = LoadState(Text);
	if (ret == 0)
		 sprintf(Text, _("*PCSX*: Loaded State %d"), num+1);
	else sprintf(Text, _("*PCSX*: Error Loading State %d"), num+1);
	GPU_displayText(Text);

	Running = 1;
	psxCpu->Execute();
}

void States_Save(int num) {
	char Text[256];
	int ret;

	SetMenu(gApp.hWnd, NULL);
	OpenPlugins(gApp.hWnd);
	if (NeedReset) {
		SysReset();
		NeedReset = 0;
	}
	GPU_updateLace();

	sprintf (Text, "sstates/%10.10s.%3.3d", CdromLabel, num);
	GPU_freeze(2, (GPUFreeze_t *)&num);
	ret = SaveState(Text);
	if (ret == 0)
		 sprintf(Text, _("*PCSX*: Loaded State %d"), num+1);
	else sprintf(Text, _("*PCSX*: Error Loading State %d"), num+1);
	GPU_displayText(Text);

	Running = 1;
	psxCpu->Execute();
}

void OnStates_LoadOther() {
	OPENFILENAME ofn;
	char szFileName[MAXFILENAME];
	char szFileTitle[MAXFILENAME];
	char szFilter[256];

	memset(&szFileName,  0, sizeof(szFileName));
	memset(&szFileTitle, 0, sizeof(szFileTitle));

	strcpy(szFilter, _("PCSX State Format"));
	strcatz(szFilter, "*.*;*.*");

    ofn.lStructSize			= sizeof(OPENFILENAME);
    ofn.hwndOwner			= gApp.hWnd;
    ofn.lpstrFilter			= szFilter;
	ofn.lpstrCustomFilter	= NULL;
    ofn.nMaxCustFilter		= 0;
    ofn.nFilterIndex		= 1;
    ofn.lpstrFile			= szFileName;
    ofn.nMaxFile			= MAXFILENAME;
    ofn.lpstrInitialDir		= NULL;
    ofn.lpstrFileTitle		= szFileTitle;
    ofn.nMaxFileTitle		= MAXFILENAME;
    ofn.lpstrTitle			= NULL;
    ofn.lpstrDefExt			= "EXE";
    ofn.Flags				= OFN_HIDEREADONLY | OFN_NOCHANGEDIR;

	if (GetOpenFileName ((LPOPENFILENAME)&ofn)) {
		char Text[256];
		int ret;

		SetMenu(gApp.hWnd, NULL);
		OpenPlugins(gApp.hWnd);
		if (NeedReset) {
			SysReset();
			NeedReset = 0;
		}

		ret = LoadState(szFileName);
		if (ret == 0)
			 sprintf(Text, _("*PCSX*: Loaded State %s"), szFileName);
		else sprintf(Text, _("*PCSX*: Error Loading State %s"), szFileName);
		GPU_displayText(Text);

		Running = 1;
		psxCpu->Execute();
	}
} 

void OnStates_Save1() { States_Save(0); } 
void OnStates_Save2() { States_Save(1); } 
void OnStates_Save3() { States_Save(2); } 
void OnStates_Save4() { States_Save(3); } 
void OnStates_Save5() { States_Save(4); } 

void OnStates_SaveOther() {
	OPENFILENAME ofn;
	char szFileName[MAXFILENAME];
	char szFileTitle[MAXFILENAME];
	char szFilter[256];

	memset(&szFileName,  0, sizeof(szFileName));
	memset(&szFileTitle, 0, sizeof(szFileTitle));

	strcpy(szFilter, _("PCSX State Format"));
	strcatz(szFilter, "*.*;*.*");

    ofn.lStructSize			= sizeof(OPENFILENAME);
    ofn.hwndOwner			= gApp.hWnd;
    ofn.lpstrFilter			= szFilter;
	ofn.lpstrCustomFilter	= NULL;
    ofn.nMaxCustFilter		= 0;
    ofn.nFilterIndex		= 1;
    ofn.lpstrFile			= szFileName;
    ofn.nMaxFile			= MAXFILENAME;
    ofn.lpstrInitialDir		= NULL;
    ofn.lpstrFileTitle		= szFileTitle;
    ofn.nMaxFileTitle		= MAXFILENAME;
    ofn.lpstrTitle			= NULL;
    ofn.lpstrDefExt			= "EXE";
    ofn.Flags				= OFN_HIDEREADONLY | OFN_NOCHANGEDIR;

	if (GetOpenFileName ((LPOPENFILENAME)&ofn)) {
		char Text[256];
		int ret;

		SetMenu(gApp.hWnd, NULL);
		OpenPlugins(gApp.hWnd);
		SysReset();
		NeedReset = 0;

		ret = SaveState(szFileName);
		if (ret == 0)
			 sprintf(Text, _("*PCSX*: Loaded State %s"), szFileName);
		else sprintf(Text, _("*PCSX*: Error Loading State %s"), szFileName);
		GPU_displayText(Text);

		Running = 1;
		psxCpu->Execute();
	}
} 

LRESULT WINAPI MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	char File[256];

	switch (msg) {
		case WM_COMMAND:
			switch (LOWORD(wParam)) {
				case ID_FILE_EXIT:
					SysClose();
					PostQuitMessage(0);
					exit(0);
					return TRUE;

				case ID_FILE_RECORD_MOVIE:
					WIN32_StartMovieRecord();
					return TRUE;

				case ID_FILE_REPLAY_MOVIE:
					WIN32_StartMovieReplay(0);
					return TRUE;

				case ID_FILE_STOP_MOVIE:
					MOV_StopMovie();
					EnableMenuItem(gApp.hMenu,ID_FILE_RECORD_MOVIE,MF_ENABLED);
					EnableMenuItem(gApp.hMenu,ID_FILE_REPLAY_MOVIE,MF_ENABLED);
					EnableMenuItem(gApp.hMenu,ID_FILE_STOP_MOVIE,MF_GRAYED);
					return TRUE;

				case ID_LUA_OPEN:
					if(!LuaConsoleHWnd)
					{
						LuaConsoleHWnd = CreateDialog(gApp.hInstance, MAKEINTRESOURCE(IDD_LUA), NULL, (DLGPROC) DlgLuaScriptDialog);
					}
					else
						SetForegroundWindow(LuaConsoleHWnd);
					break;

				case ID_LUA_CLOSE_ALL:
					if(LuaConsoleHWnd)
						PostMessage(LuaConsoleHWnd, WM_CLOSE, 0, 0);
					break;

				case ID_START_CAPTURE:
					ShellExecute(NULL, "open", "http://code.google.com/p/pcsxrr/wiki/AviHelp", NULL, NULL, SW_SHOWNORMAL);
//					WIN32_StartAviRecord();
					break;

				case ID_FILE_RUN_CD:
					LoadCdBios = 0;

					if(!OpenPlugins(hWnd)) return FALSE;
					
					SetMenu(hWnd, NULL);
					SysReset();
					NeedReset = 0;
					CheckCdrom();
					if (LoadCdrom() == -1) {
						ClosePlugins();
						RestoreWindow();
						SysMessage(_("Could not load Cdrom"));
						return TRUE;
					}
					Running = 1;
					psxCpu->Execute();
					return TRUE;

				case ID_FILE_RUNCDBIOS:
					LoadCdBios = 1;
					SetMenu(hWnd, NULL);
					OpenPlugins(hWnd);
					CheckCdrom();
					SysReset();
					NeedReset = 0;
					Running = 1;
					psxCpu->Execute();
					return TRUE;

				case ID_FILE_RUN_EXE:
					if (!Open_File_Proc(File)) return TRUE;
					SetMenu(hWnd, NULL);
					OpenPlugins(hWnd);
					SysReset();
					NeedReset = 0;
					Load(File);
					Running = 1;
					psxCpu->Execute();
					return TRUE;

				case ID_FILE_STATES_LOAD_SLOT1: States_Load(0); return TRUE;
				case ID_FILE_STATES_LOAD_SLOT2: States_Load(1); return TRUE;
				case ID_FILE_STATES_LOAD_SLOT3: States_Load(2); return TRUE;
				case ID_FILE_STATES_LOAD_SLOT4: States_Load(3); return TRUE;
				case ID_FILE_STATES_LOAD_SLOT5: States_Load(4); return TRUE;
				case ID_FILE_STATES_LOAD_OTHER: OnStates_LoadOther(); return TRUE;

				case ID_FILE_STATES_SAVE_SLOT1: States_Save(0); return TRUE;
				case ID_FILE_STATES_SAVE_SLOT2: States_Save(1); return TRUE;
				case ID_FILE_STATES_SAVE_SLOT3: States_Save(2); return TRUE;
				case ID_FILE_STATES_SAVE_SLOT4: States_Save(3); return TRUE;
				case ID_FILE_STATES_SAVE_SLOT5: States_Save(4); return TRUE;
				case ID_FILE_STATES_SAVE_OTHER: OnStates_SaveOther(); return TRUE;

				case ID_EMULATOR_RUN:
					SetMenu(hWnd, NULL);
					OpenPlugins(hWnd);
//					ShowCursor(FALSE);
					if (NeedReset) { SysReset(); NeedReset = 0; }
					Running = 1;
					psxCpu->Execute();
					return TRUE;

				case ID_EMULATOR_RESET:
					NeedReset = 1;
					return TRUE;

				case ID_CONFIGURATION_GRAPHICS:
					if (GPU_configure) GPU_configure();
					return TRUE;

				case ID_CONFIGURATION_SOUND:
					if (SPU_configure) SPU_configure();
					return TRUE;

				case ID_CONFIGURATION_CONTROLLERS:
					if (PAD1_configure) PAD1_configure();
					if (strcmp(Config.Pad1, Config.Pad2)) if (PAD2_configure) PAD2_configure();
					return TRUE;

				case ID_CONFIGURATION_CDROM:
						if (CDR_configure) CDR_configure();
					return TRUE;

				case ID_CONFIGURATION_MAPHOTKEYS:
						MHkeysCreate();
					return TRUE;

				case ID_RAM_SEARCH:
					if(!RamSearchHWnd)
					{
						reset_address_info();
						RamSearchHWnd = CreateDialog(gApp.hInstance, MAKEINTRESOURCE(IDD_RAMSEARCH), NULL, (DLGPROC) RamSearchProc);
					}
					else
						SetForegroundWindow(RamSearchHWnd);
					return TRUE;

				case ID_RAM_WATCH:
					if(!RamWatchHWnd)
					{
						RamWatchHWnd = CreateDialog(gApp.hInstance, MAKEINTRESOURCE(IDD_RAMWATCH), NULL, (DLGPROC) RamWatchProc);
					}
					else
						SetForegroundWindow(RamWatchHWnd);
					return TRUE;

				case ID_CONFIGURATION_MEMPOKE:
						CreateMemPoke();
					return TRUE;

				case ID_CONFIGURATION_CHEATS:
						PCSXRemoveCheats();
						CreateCheatEditor();
						PCSXApplyCheats();
					return TRUE;

				case ID_CONFIGURATION_NETPLAY:
					DialogBox(gApp.hInstance, MAKEINTRESOURCE(IDD_NETPLAY), hWnd, (DLGPROC)ConfigureNetPlayDlgProc);
					return TRUE;

				case ID_CONFIGURATION_MEMORYCARDMANAGER:
					DialogBox(gApp.hInstance, MAKEINTRESOURCE(IDD_MCDCONF), hWnd, (DLGPROC)ConfigureMcdsDlgProc);
					return TRUE;

				case ID_CONFIGURATION_CPU:
					DialogBox(gApp.hInstance, MAKEINTRESOURCE(IDD_CPUCONF), hWnd, (DLGPROC)ConfigureCpuDlgProc);
					return TRUE;

				case ID_CONFIGURATION:
					ConfigurePlugins(hWnd);
					return TRUE;

				case ID_HELP_TUTORIAL:
					ShellExecute(NULL, "open", "http://code.google.com/p/pcsxrr/wiki/QuickTutorial", NULL, NULL, SW_SHOWNORMAL);
					break;

				case ID_HELP_HELP:
					ShellExecute(NULL, "open", "pcsx-rr-instructions.txt", NULL, NULL, SW_SHOWNORMAL);
					return TRUE;

				case ID_HELP_ABOUT:
					DialogBox(gApp.hInstance, MAKEINTRESOURCE(ABOUT_DIALOG), hWnd, (DLGPROC)AboutDlgProc);
					return TRUE;
			}
			break;

		case WM_SYSKEYDOWN:
			if (wParam != VK_F10)
				return DefWindowProc(hWnd, msg, wParam, lParam);

		case WM_KEYDOWN:
			PADhandleKey(wParam);
			return TRUE;

		case WM_KEYUP: {
			int modifier = 0;int key;
			key = wParam;
			if(GetAsyncKeyState(VK_MENU))
				modifier = VK_MENU;
			else if(GetAsyncKeyState(VK_CONTROL))
				modifier = VK_CONTROL;
			else if(GetAsyncKeyState(VK_SHIFT))
				modifier = VK_SHIFT;
			if(key == EmuCommandTable[EMUCMD_TURBOMODE].key
			&& modifier == EmuCommandTable[EMUCMD_TURBOMODE].keymod)
			{
				SetEmulationSpeed(EMUSPEED_NORMAL);
				iTurboMode = 0;
			}
			return TRUE;
		}
	
		case WM_MOUSEMOVE: {
			mousex=LOWORD(lParam);
			mousey=HIWORD(lParam);
			return TRUE;
		}
	
		case WM_DESTROY:
			if (!AccBreak) {
				if (Movie.mode != MOVIEMODE_INACTIVE)
					MOV_StopMovie();
				if (Movie.capture)
					WIN32_StopAviRecord();
				if (Running)
					ClosePlugins();
				SysClose();
				PostQuitMessage(0);
				exit(0);
			}
			else AccBreak = 0;
			return TRUE;

		case WM_QUIT:
			if (Movie.capture)
				WIN32_StopAviRecord();
			if (Movie.mode != MOVIEMODE_INACTIVE)
				MOV_StopMovie();
			exit(0);
			break;

		default:
			return DefWindowProc(hWnd, msg, wParam, lParam);
	}

	return FALSE;
}

HWND mcdDlg;
McdBlock Blocks[2][15];
int IconC[2][15];
HIMAGELIST Iiml[2];
HICON eICON;

void CreateListView(int idc) {
	HWND List;
	LV_COLUMN col;

	List = GetDlgItem(mcdDlg, idc);

	col.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
	col.fmt  = LVCFMT_LEFT;
	
	col.pszText  = _("Title");
	col.cx       = 170;
	col.iSubItem = 0;

	ListView_InsertColumn(List, 0, &col);

	col.pszText  = _("Status");
	col.cx       = 50;
	col.iSubItem = 1;

	ListView_InsertColumn(List, 1, &col);

	col.pszText  = _("Game ID");
	col.cx       = 90;
	col.iSubItem = 2;

	ListView_InsertColumn(List, 2, &col);

	col.pszText  = _("Game");
	col.cx       = 80;
	col.iSubItem = 3;

	ListView_InsertColumn(List, 3, &col);
}

int GetRGB() {
    HDC scrDC, memDC;
    HBITMAP oldBmp = NULL; 
    HBITMAP curBmp = NULL;
    COLORREF oldColor;
    COLORREF curColor = RGB(255,255,255);
    int i, R, G, B;

    R = G = B = 1;
 
    scrDC = CreateDC("DISPLAY", NULL, NULL, NULL);
    memDC = CreateCompatibleDC(NULL); 
    curBmp = CreateCompatibleBitmap(scrDC, 1, 1);    
    oldBmp = (HBITMAP)SelectObject(memDC, curBmp);
        
    for (i = 255; i >= 0; --i) {
        oldColor = curColor;
        curColor = SetPixel(memDC, 0, 0, RGB(i, i, i));
        
        if (GetRValue(curColor) < GetRValue(oldColor)) ++R; 
        if (GetGValue(curColor) < GetGValue(oldColor)) ++G;
        if (GetBValue(curColor) < GetBValue(oldColor)) ++B;
    }
 
    DeleteObject(oldBmp);
    DeleteObject(curBmp);
    DeleteDC(scrDC);
    DeleteDC(memDC);
 
    return (R * G * B);
}
 
HICON GetIcon(short *icon) {
    ICONINFO iInfo;
    HDC hDC;
    char mask[16*16];
    int x, y, c, Depth;
  
    hDC = CreateIC("DISPLAY",NULL,NULL,NULL);
    Depth=GetDeviceCaps(hDC, BITSPIXEL);
    DeleteDC(hDC);
 
    if (Depth == 16) {
        if (GetRGB() == (32 * 32 * 32))        
            Depth = 15;
    }

    for (y=0; y<16; y++) {
        for (x=0; x<16; x++) {
            c = icon[y*16+x];
            if (Depth == 15 || Depth == 32)
				c = ((c&0x001f) << 10) | 
					((c&0x7c00) >> 10) | 
					((c&0x03e0)      );
			else
                c = ((c&0x001f) << 11) |
					((c&0x7c00) >>  9) |
					((c&0x03e0) <<  1);

            icon[y*16+x] = c;
        }
    }    

    iInfo.fIcon = TRUE;
    memset(mask, 0, 16*16);
    iInfo.hbmMask  = CreateBitmap(16, 16, 1, 1, mask);
    iInfo.hbmColor = CreateBitmap(16, 16, 1, 16, icon); 
 
    return CreateIconIndirect(&iInfo);
}

HICON hICON[2][3][15];
int aIover[2];                        
int ani[2];
 
void LoadMcdItems(int mcd, int idc) {
    HWND List = GetDlgItem(mcdDlg, idc);
    LV_ITEM item;
    HIMAGELIST iml = Iiml[mcd-1];
    int i, j;
    HICON hIcon;
    McdBlock *Info;
 
    aIover[mcd-1]=0;
    ani[mcd-1]=0;
 
    ListView_DeleteAllItems(List);

    for (i=0; i<15; i++) {
  
        item.mask      = LVIF_TEXT | LVIF_IMAGE;
        item.iItem       = i;
        item.iImage    = i;
        item.pszText  = LPSTR_TEXTCALLBACK;
        item.iSubItem = 0;
 
        IconC[mcd-1][i] = 0;
        Info = &Blocks[mcd-1][i];
 
        if ((Info->Flags & 0xF) == 1 && Info->IconCount != 0) {
            hIcon = GetIcon(Info->Icon);   
 
            if (Info->IconCount > 1) {
                for(j = 0; j < 3; j++)
                    hICON[mcd-1][j][i]=hIcon;
            }
        } else {
            hIcon = eICON; 
        }
 
        ImageList_ReplaceIcon(iml, -1, hIcon);
        ListView_InsertItem(List, &item);
    } 
}

void UpdateMcdItems(int mcd, int idc) {
    HWND List = GetDlgItem(mcdDlg, idc);
    LV_ITEM item;
    HIMAGELIST iml = Iiml[mcd-1];
    int i, j;
    McdBlock *Info;
    HICON hIcon;
 
    aIover[mcd-1]=0;
    ani[mcd-1]=0;
  
    for (i=0; i<15; i++) { 
 
        item.mask     = LVIF_TEXT | LVIF_IMAGE;
        item.iItem    = i;
        item.iImage   = i;
        item.pszText  = LPSTR_TEXTCALLBACK;
        item.iSubItem = 0;
 
        IconC[mcd-1][i] = 0; 
        Info = &Blocks[mcd-1][i];
 
        if ((Info->Flags & 0xF) == 1 && Info->IconCount != 0) {
            hIcon = GetIcon(Info->Icon);   
 
            if (Info->IconCount > 1) { 
                for(j = 0; j < 3; j++)
                    hICON[mcd-1][j][i]=hIcon;
            }
        } else { 
            hIcon = eICON; 
        }
              
        ImageList_ReplaceIcon(iml, i, hIcon);
        ListView_SetItem(List, &item);
    } 
    ListView_Update(List, -1);
}
 
void McdListGetDispInfo(int mcd, int idc, LPNMHDR pnmh) {
	LV_DISPINFO *lpdi = (LV_DISPINFO *)pnmh;
	McdBlock *Info;

	Info = &Blocks[mcd-1][lpdi->item.iItem];

	switch (lpdi->item.iSubItem) {
		case 0:
			switch (Info->Flags & 0xF) {
				case 1:
					lpdi->item.pszText = Info->Title;
					break;
				case 2:
					lpdi->item.pszText = _("mid link block");
					break;
				case 3:
					lpdi->item.pszText = _("terminiting link block");
					break;
			}
			break;
		case 1:
			if ((Info->Flags & 0xF0) == 0xA0) {
				if ((Info->Flags & 0xF) >= 1 &&
					(Info->Flags & 0xF) <= 3) {
					lpdi->item.pszText = _("Deleted");
				} else lpdi->item.pszText = _("Free");
			} else if ((Info->Flags & 0xF0) == 0x50)
				lpdi->item.pszText = _("Used");
			else { lpdi->item.pszText = _("Free"); }
			break;
		case 2:
			if((Info->Flags & 0xF)==1)
				lpdi->item.pszText = Info->ID;
			break;
		case 3:
			if((Info->Flags & 0xF)==1)
				lpdi->item.pszText = Info->Name;
			break;
	}
}

void McdListNotify(int mcd, int idc, LPNMHDR pnmh) {
	switch (pnmh->code) {
		case LVN_GETDISPINFO: McdListGetDispInfo(mcd, idc, pnmh); break;
	}
}

void UpdateMcdDlg() {
	int i;

	for (i=1; i<16; i++) GetMcdBlockInfo(1, i, &Blocks[0][i-1]);
	for (i=1; i<16; i++) GetMcdBlockInfo(2, i, &Blocks[1][i-1]);
	UpdateMcdItems(1, IDC_LIST1);
	UpdateMcdItems(2, IDC_LIST2);
}

void LoadMcdDlg() {
	int i;

	for (i=1; i<16; i++) GetMcdBlockInfo(1, i, &Blocks[0][i-1]);
	for (i=1; i<16; i++) GetMcdBlockInfo(2, i, &Blocks[1][i-1]);
	LoadMcdItems(1, IDC_LIST1);
	LoadMcdItems(2, IDC_LIST2);
}

void UpdateMcdIcon(int mcd, int idc) {
    HWND List = GetDlgItem(mcdDlg, idc);
    HIMAGELIST iml = Iiml[mcd-1];
    int i;
    McdBlock *Info;
    int *count; 
 
    if(!aIover[mcd-1]) {
        ani[mcd-1]++; 
 
        for (i=0; i<15; i++) {
            Info = &Blocks[mcd-1][i];
            count = &IconC[mcd-1][i];
 
            if ((Info->Flags & 0xF) != 1) continue;
            if (Info->IconCount <= 1) continue;
 
            if (*count < Info->IconCount) {
                (*count)++;
                aIover[mcd-1]=0;
 
                if(ani[mcd-1] <= (Info->IconCount-1))  // last frame and below...
                    hICON[mcd-1][ani[mcd-1]][i] = GetIcon(&Info->Icon[(*count)*16*16]);
            } else {
                aIover[mcd-1]=1;
            }
        }

    } else { 
 
        if (ani[mcd-1] > 1) ani[mcd-1] = 0;  // 1st frame
        else ani[mcd-1]++;                       // 2nd, 3rd frame
 
        for(i=0;i<15;i++) {
            Info = &Blocks[mcd-1][i];
 
            if (((Info->Flags & 0xF) == 1) && (Info->IconCount > 1))
                ImageList_ReplaceIcon(iml, i, hICON[mcd-1][ani[mcd-1]][i]);
        }
        InvalidateRect(List,  NULL, FALSE);
    }
}

static int copy = 0, copymcd = 0;
//static int listsel = 0;

BOOL CALLBACK ConfigureMcdsDlgProc(HWND hW, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	char str[256];
	LPBYTE lpAND, lpXOR;
	LPBYTE lpA, lpX;
	int i, j;

	switch(uMsg) {
		case WM_INITDIALOG:
			mcdDlg = hW;

			SetWindowText(hW, _("Memcard Manager"));

			Button_SetText(GetDlgItem(hW, IDOK),        _("OK"));
			Button_SetText(GetDlgItem(hW, IDCANCEL),    _("Cancel"));
			Button_SetText(GetDlgItem(hW, IDC_MCDSEL1), _("Select Mcd"));
			Button_SetText(GetDlgItem(hW, IDC_FORMAT1), _("Format Mcd"));
			Button_SetText(GetDlgItem(hW, IDC_RELOAD1), _("Reload Mcd"));
			Button_SetText(GetDlgItem(hW, IDC_MCDSEL2), _("Select Mcd"));
			Button_SetText(GetDlgItem(hW, IDC_FORMAT2), _("Format Mcd"));
			Button_SetText(GetDlgItem(hW, IDC_RELOAD2), _("Reload Mcd"));
			Button_SetText(GetDlgItem(hW, IDC_COPYTO2), _("-> Copy ->"));
			Button_SetText(GetDlgItem(hW, IDC_COPYTO1), _("<- Copy <-"));
			Button_SetText(GetDlgItem(hW, IDC_PASTE),   _("Paste"));
			Button_SetText(GetDlgItem(hW, IDC_DELETE1), _("<- Un/Delete"));
			Button_SetText(GetDlgItem(hW, IDC_DELETE2), _("Un/Delete ->"));
 
			Static_SetText(GetDlgItem(hW, IDC_FRAMEMCD1), _("Memory Card 1"));
			Static_SetText(GetDlgItem(hW, IDC_FRAMEMCD2), _("Memory Card 2"));

			lpA=lpAND=(LPBYTE)HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,(16*16));
			lpX=lpXOR=(LPBYTE)HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,(16*16));

			for(i=0;i<16;i++)
			{
				for(j=0;j<16;j++)
				{
					*lpA++=0xff;
					*lpX++=0;
				}
			}
			eICON=CreateIcon(gApp.hInstance,16,16,1,1,lpAND,lpXOR);

			HeapFree(GetProcessHeap(),0,lpAND);
			HeapFree(GetProcessHeap(),0,lpXOR);

			if (!strlen(Config.Mcd1)) strcpy(Config.Mcd1, "memcards\\Mcd001.mcr");
			if (!strlen(Config.Mcd2)) strcpy(Config.Mcd2, "memcards\\Mcd002.mcr");
			Edit_SetText(GetDlgItem(hW,IDC_MCD1), Config.Mcd1);
			Edit_SetText(GetDlgItem(hW,IDC_MCD2), Config.Mcd2);

			CreateListView(IDC_LIST1);
			CreateListView(IDC_LIST2);

			Iiml[0] = ImageList_Create(16, 16, ILC_COLOR16, 0, 0);
			Iiml[1] = ImageList_Create(16, 16, ILC_COLOR16, 0, 0);

			_IMAGELIST* temp;
			temp = ListView_SetImageList(GetDlgItem(mcdDlg, IDC_LIST1), Iiml[0], LVSIL_SMALL);
			temp = ListView_SetImageList(GetDlgItem(mcdDlg, IDC_LIST2), Iiml[1], LVSIL_SMALL);

			Button_Enable(GetDlgItem(hW, IDC_PASTE), FALSE);

			LoadMcdDlg();

			SetTimer(hW, 1, 250, NULL);

			return TRUE;

		case WM_COMMAND:
			switch (LOWORD(wParam)) {
				case IDC_COPYTO1:
					copy = ListView_GetSelectionMark(GetDlgItem(mcdDlg, IDC_LIST2));
					copymcd = 1;

					Button_Enable(GetDlgItem(hW, IDC_PASTE), TRUE);
					return TRUE;
				case IDC_COPYTO2:
					copy = ListView_GetSelectionMark(GetDlgItem(mcdDlg, IDC_LIST1));
					copymcd = 2;

					Button_Enable(GetDlgItem(hW, IDC_PASTE), TRUE);
					return TRUE;
				case IDC_PASTE:
					if (MessageBox(hW, _("Are you sure you want to paste this selection?"), _("Confirmation"), MB_YESNO) == IDNO) return TRUE;

					if (copymcd == 1) {
						Edit_GetText(GetDlgItem(hW,IDC_MCD1), str, 256);
						i = ListView_GetSelectionMark(GetDlgItem(mcdDlg, IDC_LIST1));

						// save dir data + save data
						memcpy(Mcd1Data + (i+1) * 128, Mcd2Data + (copy+1) * 128, 128);
						SaveMcd(str, Mcd1Data, (i+1) * 128, 128);
						memcpy(Mcd1Data + (i+1) * 1024 * 8, Mcd2Data + (copy+1) * 1024 * 8, 1024 * 8);
						SaveMcd(str, Mcd1Data, (i+1) * 1024 * 8, 1024 * 8);
					} else { // 2
						Edit_GetText(GetDlgItem(hW,IDC_MCD2), str, 256);
						i = ListView_GetSelectionMark(GetDlgItem(mcdDlg, IDC_LIST2));

						// save dir data + save data
						memcpy(Mcd2Data + (i+1) * 128, Mcd1Data + (copy+1) * 128, 128);
						SaveMcd(str, Mcd2Data, (i+1) * 128, 128);
						memcpy(Mcd2Data + (i+1) * 1024 * 8, Mcd1Data + (copy+1) * 1024 * 8, 1024 * 8);
						SaveMcd(str, Mcd2Data, (i+1) * 1024 * 8, 1024 * 8);
					}

					UpdateMcdDlg();

					return TRUE;
				case IDC_DELETE1:
				{
					McdBlock *Info;
					int mcd = 1;
					int i, cxor = 0, j;
					unsigned char *data, *ptr;

					Edit_GetText(GetDlgItem(hW,IDC_MCD1), (LPSTR) str, 256);
					i = ListView_GetSelectionMark(GetDlgItem(mcdDlg, IDC_LIST1));
					data = (unsigned char*) Mcd1Data;

					i++;

					ptr = data + i * 128;

					Info = &Blocks[mcd-1][i-1];

					if ((Info->Flags & 0xF0) == 0xA0) {
						if ((Info->Flags & 0xF) >= 1 &&
							(Info->Flags & 0xF) <= 3) { // deleted
							*ptr = 0x50 | (Info->Flags & 0xF);
						} else return TRUE;
					} else if ((Info->Flags & 0xF0) == 0x50) { // used
						*ptr = 0xA0 | (Info->Flags & 0xF);
					} else { return TRUE; }

					for (j=0; j<127; j++) cxor^=*ptr++;
					*ptr = (unsigned) cxor;

					SaveMcd(str, (char*) data, i * 128, 128);
					UpdateMcdDlg();
				}

					return TRUE;
				case IDC_DELETE2:
				{
					McdBlock *Info;
					int mcd = 2;
					int i, cxor = 0, j;
					unsigned char *data, *ptr;

					Edit_GetText(GetDlgItem(hW,IDC_MCD2), (LPSTR) str, 256);
					i = ListView_GetSelectionMark(GetDlgItem(mcdDlg, IDC_LIST2));
					data = (unsigned char*) Mcd2Data;

					i++;

					ptr = data + i * 128;

					Info = &Blocks[mcd-1][i-1];

					if ((Info->Flags & 0xF0) == 0xA0) {
						if ((Info->Flags & 0xF) >= 1 &&
							(Info->Flags & 0xF) <= 3) { // deleted
							*ptr = 0x50 | (Info->Flags & 0xF);
						} else return TRUE;
					} else if ((Info->Flags & 0xF0) == 0x50) { // used
						*ptr = 0xA0 | (Info->Flags & 0xF);
					} else { return TRUE; }

					for (j=0; j<127; j++) cxor^=*ptr++;
					*ptr = (unsigned) cxor;

					SaveMcd(str, (char*) data, i * 128, 128);
					UpdateMcdDlg();
				}

					return TRUE;

				case IDC_MCDSEL1: 
					Open_Mcd_Proc(hW, 1); 
					return TRUE;
				case IDC_MCDSEL2: 
					Open_Mcd_Proc(hW, 2); 
					return TRUE;
				case IDC_RELOAD1: 
					Edit_GetText(GetDlgItem(hW,IDC_MCD1), str, 256);
					LoadMcd(1, str);
					UpdateMcdDlg();
					return TRUE;
				case IDC_RELOAD2: 
					Edit_GetText(GetDlgItem(hW,IDC_MCD2), str, 256);
					LoadMcd(2, str);
					UpdateMcdDlg();
					return TRUE;
				case IDC_FORMAT1:
					if (MessageBox(hW, _("Are you sure you want to format this Memory Card?"), _("Confirmation"), MB_YESNO) == IDNO) return TRUE;
					Edit_GetText(GetDlgItem(hW,IDC_MCD1), str, 256);
					CreateMcd(str);
					LoadMcd(1, str);
					UpdateMcdDlg();
					return TRUE;
				case IDC_FORMAT2:
					if (MessageBox(hW, _("Are you sure you want to format this Memory Card?"), _("Confirmation"), MB_YESNO) == IDNO) return TRUE;
					Edit_GetText(GetDlgItem(hW,IDC_MCD2), str, 256);
					CreateMcd(str);
					LoadMcd(2, str);
					UpdateMcdDlg();
					return TRUE;
       			case IDCANCEL:
					LoadMcds(Config.Mcd1, Config.Mcd2);

					EndDialog(hW,FALSE);

					return TRUE;
       			case IDOK:
					Edit_GetText(GetDlgItem(hW,IDC_MCD1), Config.Mcd1, 256);
					Edit_GetText(GetDlgItem(hW,IDC_MCD2), Config.Mcd2, 256);

					LoadMcds(Config.Mcd1, Config.Mcd2);
					SaveConfig();

					EndDialog(hW,TRUE);

					return TRUE;
			}
		case WM_NOTIFY:
			switch (wParam) {
				case IDC_LIST1: McdListNotify(1, IDC_LIST1, (LPNMHDR)lParam); break;
				case IDC_LIST2: McdListNotify(2, IDC_LIST2, (LPNMHDR)lParam); break;
			}
			return TRUE;
		case WM_TIMER:
			UpdateMcdIcon(1, IDC_LIST1);
			UpdateMcdIcon(2, IDC_LIST2);
			return TRUE;
		case WM_DESTROY:
			DestroyIcon(eICON);
			//KillTimer(hW, 1);
			return TRUE;
	}
	return FALSE;
}

BOOL CALLBACK ConfigureCpuDlgProc(HWND hW, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	long tmp;

	switch(uMsg) {
		case WM_INITDIALOG:
			Button_SetCheck(GetDlgItem(hW,IDC_XA),      Config.Xa);
			Button_SetCheck(GetDlgItem(hW,IDC_SIO),     Config.Sio);
			Button_SetCheck(GetDlgItem(hW,IDC_MDEC),    Config.Mdec);
			Button_SetCheck(GetDlgItem(hW,IDC_QKEYS),   Config.QKeys);
			Button_SetCheck(GetDlgItem(hW,IDC_CDDA),    Config.Cdda);
			Button_SetCheck(GetDlgItem(hW,IDC_PSXAUTO), Config.PsxAuto);
			Button_SetCheck(GetDlgItem(hW,IDC_CPU),     Config.Cpu);
			Button_SetCheck(GetDlgItem(hW,IDC_PAUSE),   Config.PauseAfterPlayback);
			Button_SetCheck(GetDlgItem(hW,IDC_PSXOUT),  Config.PsxOut);
			Button_SetCheck(GetDlgItem(hW,IDC_SPUIRQ),  Config.SpuIrq);
			Button_SetCheck(GetDlgItem(hW,IDC_RCNTFIX), Config.RCntFix);
			Button_SetCheck(GetDlgItem(hW,IDC_VSYNCWA), Config.VSyncWA);
			ComboBox_AddString(GetDlgItem(hW,IDC_PSXTYPES),"NTSC");
			ComboBox_AddString(GetDlgItem(hW,IDC_PSXTYPES),"PAL");
			ComboBox_SetCurSel(GetDlgItem(hW,IDC_PSXTYPES),Config.PsxType);

		case WM_COMMAND: {
			switch (LOWORD(wParam)) {
				case IDCANCEL: EndDialog(hW,FALSE); return TRUE;
				case IDOK:
					tmp = ComboBox_GetCurSel(GetDlgItem(hW,IDC_PSXTYPES));
					if (tmp == 0) Config.PsxType = 0;
					else Config.PsxType = 1;

					Config.Xa      = Button_GetCheck(GetDlgItem(hW,IDC_XA));
					Config.Sio     = Button_GetCheck(GetDlgItem(hW,IDC_SIO));
					Config.Mdec    = Button_GetCheck(GetDlgItem(hW,IDC_MDEC));
					Config.QKeys   = Button_GetCheck(GetDlgItem(hW,IDC_QKEYS));
					Config.Cdda    = Button_GetCheck(GetDlgItem(hW,IDC_CDDA));
					Config.PsxAuto = Button_GetCheck(GetDlgItem(hW,IDC_PSXAUTO));
					Config.PauseAfterPlayback = Button_GetCheck(GetDlgItem(hW,IDC_PAUSE));
					tmp = Config.Cpu;
					Config.Cpu     = Button_GetCheck(GetDlgItem(hW,IDC_CPU));
					if (tmp != Config.Cpu) {
						psxCpu->Shutdown();
						if (Config.Cpu)	
							 psxCpu = &psxInt;
						else psxCpu = &psxRec;
						if (psxCpu->Init() == -1) {
							SysClose();
							exit(1);
						}
						psxCpu->Reset();
					}
					Config.PsxOut  = Button_GetCheck(GetDlgItem(hW,IDC_PSXOUT));
					Config.SpuIrq  = Button_GetCheck(GetDlgItem(hW,IDC_SPUIRQ));
					Config.RCntFix = Button_GetCheck(GetDlgItem(hW,IDC_RCNTFIX));
					Config.VSyncWA = Button_GetCheck(GetDlgItem(hW,IDC_VSYNCWA));

					SaveConfig();

					EndDialog(hW,TRUE);

					//if (Config.PsxOut) OpenConsole();
					//else CloseConsole();

					return TRUE;
			}
		}
	}
	return FALSE;
}

void Open_Mcd_Proc(HWND hW, int mcd) {
	OPENFILENAME ofn;
	char szFileName[MAXFILENAME];
	char szFileTitle[MAXFILENAME];
	char szFilter[1024];
	char *str;

	memset(&szFileName,  0, sizeof(szFileName));
	memset(&szFileTitle, 0, sizeof(szFileTitle));
	memset(&szFilter,    0, sizeof(szFilter));

	strcpy(szFilter, _("Psx Mcd Format (*.mcr;*.mc;*.mem;*.vgs;*.mcd;*.gme;*.ddf)"));
	str = szFilter + strlen(szFilter) + 1; 
	strcpy(str, "*.mcr;*.mcd;*.mem;*.gme;*.mc;*.ddf");

	str+= strlen(str) + 1;
	strcpy(str, _("Psx Memory Card (*.mcr;*.mc)"));
	str+= strlen(str) + 1;
	strcpy(str, "*.mcr;0*.mc");

	str+= strlen(str) + 1;
	strcpy(str, _("CVGS Memory Card (*.mem;*.vgs)"));
	str+= strlen(str) + 1;
	strcpy(str, "*.mem;*.vgs");

	str+= strlen(str) + 1;
	strcpy(str, _("Bleem Memory Card (*.mcd)"));
	str+= strlen(str) + 1;
	strcpy(str, "*.mcd");

	str+= strlen(str) + 1;
	strcpy(str, _("DexDrive Memory Card (*.gme)"));
	str+= strlen(str) + 1;
	strcpy(str, "*.gme");

	str+= strlen(str) + 1;
	strcpy(str, _("DataDeck Memory Card (*.ddf)"));
	str+= strlen(str) + 1;
	strcpy(str, "*.ddf");

	str+= strlen(str) + 1;
	strcpy(str, _("All Files"));
	str+= strlen(str) + 1;
	strcpy(str, "*.*");

    ofn.lStructSize			= sizeof(OPENFILENAME);
    ofn.hwndOwner			= hW;
    ofn.lpstrFilter			= szFilter;
	ofn.lpstrCustomFilter	= NULL;
    ofn.nMaxCustFilter		= 0;
    ofn.nFilterIndex		= 1;
    ofn.lpstrFile			= szFileName;
    ofn.nMaxFile			= MAXFILENAME;
    ofn.lpstrInitialDir		= "memcards";
    ofn.lpstrFileTitle		= szFileTitle;
    ofn.nMaxFileTitle		= MAXFILENAME;
    ofn.lpstrTitle			= NULL;
    ofn.lpstrDefExt			= "MCR";
    ofn.Flags				= OFN_HIDEREADONLY | OFN_NOCHANGEDIR;

	if (GetOpenFileName ((LPOPENFILENAME)&ofn)) {
		Edit_SetText(GetDlgItem(hW,mcd == 1 ? IDC_MCD1 : IDC_MCD2), szFileName);
		LoadMcd(mcd, szFileName);
		UpdateMcdDlg();
	}
}

int Open_File_Proc(char *file) {
	OPENFILENAME ofn;
	char szFileName[MAXFILENAME];
	char szFileTitle[MAXFILENAME];
	char szFilter[256];

	memset(&szFileName,  0, sizeof(szFileName));
	memset(&szFileTitle, 0, sizeof(szFileTitle));

    ofn.lStructSize			= sizeof(OPENFILENAME);
    ofn.hwndOwner			= gApp.hWnd;

	strcpy(szFilter, _("Psx Exe Format"));
	strcatz(szFilter, "*.*;*.*");

    ofn.lpstrFilter			= szFilter;
	ofn.lpstrCustomFilter	= NULL;
    ofn.nMaxCustFilter		= 0;
    ofn.nFilterIndex		= 1;
    ofn.lpstrFile			= szFileName;
    ofn.nMaxFile			= MAXFILENAME;
    ofn.lpstrInitialDir		= NULL;
    ofn.lpstrFileTitle		= szFileTitle;
    ofn.nMaxFileTitle		= MAXFILENAME;
    ofn.lpstrTitle			= NULL;
    ofn.lpstrDefExt			= "EXE";
    ofn.Flags				= OFN_HIDEREADONLY | OFN_NOCHANGEDIR;

	if (GetOpenFileName ((LPOPENFILENAME)&ofn)) {
		strcpy(file, szFileName);
		return 1;
	} else
		return 0;
}

#define _ADDSUBMENU(menu, menun, string) \
	submenu[menun] = CreatePopupMenu(); \
	AppendMenu(menu, MF_STRING | MF_POPUP, (UINT)submenu[menun], string);

#define ADDSUBMENU(menun, string) \
	_ADDSUBMENU(gApp.hMenu, menun, string);

#define ADDSUBMENUS(submn, menun, string) \
	submenu[menun] = CreatePopupMenu(); \
	InsertMenu(submenu[submn], 0, MF_BYPOSITION | MF_STRING | MF_POPUP, (UINT)submenu[menun], string);

#define ADDMENUITEM(menun, string, id) \
	item.fType = MFT_STRING; \
	item.fMask = MIIM_STATE | MIIM_TYPE | MIIM_ID; \
	item.fState = MFS_ENABLED; \
	item.wID = id; \
	sprintf(buf, string); \
	InsertMenuItem(submenu[menun], 0, TRUE, &item);

#define ADDMENUITEMC(menun, string, id) \
	item.fType = MFT_STRING; \
	item.fMask = MIIM_STATE | MIIM_TYPE | MIIM_ID; \
	item.fState = MFS_ENABLED | MFS_CHECKED; \
	item.wID = id; \
	sprintf(buf, string); \
	InsertMenuItem(submenu[menun], 0, TRUE, &item);

#define ADDSEPARATOR(menun) \
	item.fMask = MIIM_TYPE; \
	item.fType = MFT_SEPARATOR; \
	InsertMenuItem(submenu[menun], 0, TRUE, &item);

void CreateMainMenu() {
	MENUITEMINFO item;
	HMENU submenu[256];
	char buf[256];

	item.cbSize = sizeof(MENUITEMINFO);
	item.dwTypeData = buf;
	item.cch = 256;

	gApp.hMenu = CreateMenu();

	ADDSUBMENU(0, _("&File"));
	ADDMENUITEM(0, _("E&xit"), ID_FILE_EXIT);
	ADDSEPARATOR(0);
	ADDSUBMENUS(0, 2, _("&Lua Scripting"));
	ADDMENUITEM(2, _("&Close All Script Windows"), ID_LUA_CLOSE_ALL);
	ADDMENUITEM(2, _("&New Lua Script Window..."), ID_LUA_OPEN);
	ADDSUBMENUS(0, 1, _("&Movie"));
	ADDMENUITEM(1, _("Record &AVI Help..."), ID_START_CAPTURE);
	ADDSEPARATOR(1);
	ADDMENUITEM(1, _("S&top Movie"), ID_FILE_STOP_MOVIE);
	ADDMENUITEM(1, _("Start &Playback..."), ID_FILE_REPLAY_MOVIE);
	ADDMENUITEM(1, _("Start &Recording..."), ID_FILE_RECORD_MOVIE);
	ADDSEPARATOR(0);
	ADDMENUITEM(0, _("Run &EXE"), ID_FILE_RUN_EXE);
	ADDMENUITEM(0, _("Run CD Through &Bios"), ID_FILE_RUNCDBIOS);
	ADDMENUITEM(0, _("Run &CD"), ID_FILE_RUN_CD);

	ADDSUBMENU(0, _("&Emulator"));
	ADDMENUITEM(0, _("Re&set"), ID_EMULATOR_RESET);
	ADDMENUITEM(0, _("&Run"), ID_EMULATOR_RUN);

	ADDSUBMENU(0, _("&Configuration"));
	ADDMENUITEM(0, _("&Options"), ID_CONFIGURATION_CPU);
	ADDSEPARATOR(0);
	ADDMENUITEM(0, _("&NetPlay"), ID_CONFIGURATION_NETPLAY);
	ADDSEPARATOR(0);
	ADDMENUITEM(0, _("&Controllers"), ID_CONFIGURATION_CONTROLLERS);
	ADDMENUITEM(0, _("CD-&ROM"), ID_CONFIGURATION_CDROM);
	ADDMENUITEM(0, _("&Sound"), ID_CONFIGURATION_SOUND);
	ADDMENUITEM(0, _("&Graphics"), ID_CONFIGURATION_GRAPHICS);
	ADDSEPARATOR(0);
	ADDMENUITEM(0, _("&Plugins && Bios"), ID_CONFIGURATION);

	ADDSUBMENU(0, _("&Tools"));
	ADDMENUITEM(0, _("Map &Hotkeys"), ID_CONFIGURATION_MAPHOTKEYS);
	ADDSEPARATOR(0);
	ADDMENUITEM(0, _("RAM &Watch"), ID_RAM_WATCH);
	ADDMENUITEM(0, _("RAM &Poke"), ID_CONFIGURATION_MEMPOKE);
	ADDMENUITEM(0, _("RAM &Search"), ID_RAM_SEARCH);
	ADDMENUITEM(0, _("&Cheat Editor"), ID_CONFIGURATION_CHEATS);
	ADDSEPARATOR(0);
	ADDMENUITEM(0, _("&Memory Cards"), ID_CONFIGURATION_MEMORYCARDMANAGER);

	ADDSUBMENU(0, _("&Help"));
	ADDMENUITEM(0, _("&About..."), ID_HELP_ABOUT);
	ADDSEPARATOR(0);
	ADDMENUITEM(0, _("&TAS Help"), ID_HELP_HELP);
	ADDMENUITEM(0, _("&Quick Tutorial"), ID_HELP_TUTORIAL);

	EnableMenuItem(gApp.hMenu,ID_FILE_STOP_MOVIE,MF_GRAYED);
	EnableMenuItem(gApp.hMenu,ID_LUA_CLOSE_ALL,MF_GRAYED);
}

void CreateMainWindow(int nCmdShow) {
	WNDCLASS wc;
	HWND hWnd;
	HBRUSH hBrush;
	COLORREF crBkg = RGB(14,90,193);
	hBrush=CreateSolidBrush(crBkg);

	wc.lpszClassName = "PCSX Main";
	wc.lpfnWndProc = MainWndProc;
	wc.style = 0;
	wc.hInstance = gApp.hInstance;
	wc.hIcon = LoadIcon(gApp.hInstance, MAKEINTRESOURCE(IDI_APP_ICON));
	wc.hCursor = NULL;
	wc.hbrBackground = hBrush;
	wc.lpszMenuName = 0;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;

	RegisterClass(&wc);


	hWnd = CreateWindow("PCSX Main",
						PCSXRR_NAME_AND_VERSION,
						WS_CAPTION | WS_POPUPWINDOW | WS_MINIMIZEBOX,
						20,
						20,
						320,
						240,
						NULL,
						NULL,
						gApp.hInstance,
						NULL);

	gApp.hWnd = hWnd;
	ResetMenuSlots();

	CreateMainMenu();
	SetMenu(gApp.hWnd, gApp.hMenu);

	if (Movie.mode != MOVIEMODE_INACTIVE) {
		EnableMenuItem(gApp.hMenu,ID_FILE_RECORD_MOVIE,MF_GRAYED);
		EnableMenuItem(gApp.hMenu,ID_FILE_REPLAY_MOVIE,MF_GRAYED);
		EnableMenuItem(gApp.hMenu,ID_FILE_STOP_MOVIE,MF_ENABLED);
	}
//	if (Movie.capture) {
//		EnableMenuItem(gApp.hMenu,ID_END_CAPTURE,MF_ENABLED);
//		EnableMenuItem(gApp.hMenu,ID_START_CAPTURE,MF_GRAYED);
//	}

	ShowWindow(hWnd, nCmdShow);
}

WIN32_FIND_DATA lFindData;
HANDLE lFind;
int lFirst;

int SysInit() {
	//if (Config.PsxOut) 
		//OpenConsole();

	if (psxInit() == -1) return -1;

	#ifdef GTE_DUMP
	gteLog = fopen("gteLog.txt","w");
	#endif

#ifdef EMU_LOG
	emuLog = fopen("emuLog.txt","w");
	setvbuf(emuLog, NULL,  _IONBF, 0);
#endif

	while (LoadPlugins() == -1) {
		CancelQuit = 1;
		ConfigurePlugins(gApp.hWnd);
		CancelQuit = 0;
	}
	LoadMcds(Config.Mcd1, Config.Mcd2);

	return 0;
}

void SysReset() {
	psxReset();
}


void SysClose() {
	psxShutdown();
	ReleasePlugins();

	if (Config.PsxOut) CloseConsole();

	if (emuLog != NULL) fclose(emuLog);
	#ifdef GTE_DUMP
	if (gteLog != NULL) fclose(gteLog);
	#endif
}

void SysPrintf(char *fmt, ...) {
	va_list list;
	char msg[512];
	DWORD tmp;

	if (!hConsole) return;

	va_start(list,fmt);
	vsprintf(msg,fmt,list);
	va_end(list);

	WriteConsole(hConsole, msg, (DWORD)strlen(msg), &tmp, 0);
#ifdef EMU_LOG
#ifndef LOG_STDOUT
	fprintf(emuLog, "%s", msg);
#endif
#endif
}

void SysMessage(char *fmt, ...) {
	va_list list;
	char tmp[512];

	va_start(list,fmt);
	vsprintf(tmp,fmt,list);
	va_end(list);
	MessageBox(0, tmp, _("PCSX-RR Message"), 0);
}

static char *err = N_("Error Loading Symbol");
static int errval;

void *SysLoadLibrary(char *lib) {
	return LoadLibrary(lib);
}

void *SysLoadSym(void *lib, char *sym) {
	void *tmp = (void *)GetProcAddress((HINSTANCE)lib, sym);
	if (tmp == NULL) errval = 1;
	else errval = 0;
	return tmp;
}

const char *SysLibError() {
	if (errval) { errval = 0; return err; }
	return NULL;
}

void SysCloseLibrary(void *lib) {
	FreeLibrary((HINSTANCE)lib);
}

void SysUpdate() {
	RunMessageLoop();
}

void SysRunGui() {
	RestoreWindow();
	RunGui();
}


void WIN32_StartMovieReplay(char* szFilename)
{
	int nRet=1;
	CheckCdrom(); //get CdromId
	if (szFilename != '\0') {
		if (! MOV_ReadMovieFile(szFilename,&Movie) ) {
			char errorMessage[300];
			sprintf(errorMessage, "Movie file \"%s\" doesn't exist.",szFilename);
			MessageBox(NULL, errorMessage, NULL, MB_ICONERROR);
			return;
		}
		GetMovieFilenameMini(Movie.movieFilename);
	}
	else
		nRet = MOV_W32_StartReplayDialog();
	if (nRet) {
		LoadCdBios = 0;
		SetMenu(gApp.hWnd, NULL);
		OpenPlugins(gApp.hWnd);
		SysReset();
		NeedReset = 0;
		CheckCdrom();
		if (LoadCdrom() == -1) {
			ClosePlugins();
			RestoreWindow();
			SysMessage(_("Could not load Cdrom"));
			return;
		}
		Running = 1;
		MOV_StartMovie(MOVIEMODE_PLAY);
		psxCpu->Execute();
	}
}

void WIN32_StartMovieRecord()
{
	int nRet;
	CheckCdrom(); //get CdromId
	nRet = MOV_W32_StartRecordDialog();
	if (nRet) {
		if (Movie.saveStateIncluded) {
			SetMenu(gApp.hWnd, NULL);
			OpenPlugins(gApp.hWnd);
			Running = 1;
			MOV_StartMovie(MOVIEMODE_RECORD);
			psxCpu->Execute();
			return;
		}
		LoadCdBios = 0;
		SetMenu(gApp.hWnd, NULL);
		OpenPlugins(gApp.hWnd);
		SysReset();
		NeedReset = 0;
		CheckCdrom();
		if (LoadCdrom() == -1) {
			ClosePlugins();
			RestoreWindow();
			SysMessage(_("Could not load Cdrom"));
			return;
		}
		Running = 1;
		MOV_StartMovie(MOVIEMODE_RECORD);
		psxCpu->Execute();
	}
}

void WIN32_StartAviRecord()
{
	char nameo[256];
	const char filter[]="AVI Files (*.avi)\0*.avi\0";
	OPENFILENAME ofn;
	char fszDrive[256];
	char fszDirectory[256];
	char fszFilename[256];
	char fszExt[256];

	if (Movie.capture)
		return;

	memset(&ofn,0,sizeof(ofn));
	ofn.lStructSize=sizeof(ofn);
	ofn.hInstance=gApp.hInstance;
	ofn.lpstrTitle="Save AVI as...";
	ofn.lpstrFilter=filter;
	nameo[0]=0;
	ofn.lpstrFile=nameo;
	ofn.nMaxFile=256;
	ofn.Flags=OFN_EXPLORER|OFN_HIDEREADONLY|OFN_OVERWRITEPROMPT;
	ofn.lpstrInitialDir=".\\";
	if(GetSaveFileName(&ofn)) {
		int i;
		i = strlen(nameo);

		//add .avi if nameo doesn't have it
		if((i < 4 || nameo[i-4] != '.') && (i < (256-8))) {
			nameo[i] = '.';
			nameo[i+1] = 'a';
			nameo[i+2] = 'v';
			nameo[i+3] = 'i';
			nameo[i+4] = 0;
		}
	}
	else
		return;

	Movie.capture = 1;
	EnableMenuItem(gApp.hMenu,ID_START_CAPTURE,MF_GRAYED);
	fszDrive[0] = '\0';
	fszDirectory[0] = '\0';
	fszFilename[0] = '\0';
	fszExt[0] = '\0';
	_splitpath(nameo, fszDrive, fszDirectory, fszFilename, fszExt);

	sprintf(Movie.aviFilename, "%s%s%s.avi",fszDrive,fszDirectory,fszFilename);
	sprintf(Movie.wavFilename, "%s%s%s.wav",fszDrive,fszDirectory,fszFilename);
	SetMenu(gApp.hWnd, NULL);
	OpenPlugins(gApp.hWnd);
	GPU_startAvi(Movie.aviFilename);
	SPU_startWav(Movie.wavFilename);
	if (NeedReset) { SysReset(); NeedReset = 0; }
	Running = 1;
	psxCpu->Execute();
}

void WIN32_StopAviRecord()
{
	if (!Movie.capture)
		return;
	GPU_stopAvi();
	SPU_stopWav();
	Movie.capture = 0;
	EnableMenuItem(gApp.hMenu,ID_START_CAPTURE,MF_ENABLED);
}


INT_PTR CALLBACK DlgMemPoke(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch(msg) {
		case WM_INITDIALOG: {
			SetDlgItemText(hDlg, IDC_NC_ADDRESS, bufPokeAddress);
			SetDlgItemText(hDlg, IDC_NC_NEWVAL, bufPokeNewval);
		}
		return TRUE;
		case WM_DESTROY: {
			memPokeHWND = NULL;
			GetDlgItemText(hDlg, IDC_NC_ADDRESS, bufPokeAddress, 7);
			GetDlgItemText(hDlg, IDC_NC_NEWVAL, bufPokeNewval, 7);
			return TRUE;
		}
		case WM_COMMAND: {
			switch(LOWORD(wParam)) {

				case IDC_C_ADD: {
					uint32 address;
					uint8 newval;
					unsigned int scanned;
					GetDlgItemText(hDlg, IDC_NC_ADDRESS, bufPokeAddress, 7);
					GetDlgItemText(hDlg, IDC_NC_NEWVAL, bufPokeNewval, 7);
					ScanAddress(bufPokeAddress,&address);
					sscanf(bufPokeNewval, "%d", &scanned);
					newval = (uint8)(scanned & 0xff);
					psxMemWrite8(address,newval);
					Update_RAM_Search();
					return TRUE;
				}

				case IDCANCEL:
					DestroyWindow(hDlg);
					return TRUE;

				default:
					break;
			}
		}
		default:
			return FALSE;
	}
}

void CreateMemPoke()
{
	if(!memPokeHWND) { // create and show non-modal cheat search window
		memPokeHWND = CreateDialog(gApp.hInstance, MAKEINTRESOURCE(IDD_RAM_POKE), NULL, DlgMemPoke);
		ShowWindow(memPokeHWND, SW_SHOW);
	}
	else // already open so just reactivate the window
		SetActiveWindow(memPokeHWND);
}

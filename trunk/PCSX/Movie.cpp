#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "PsxCommon.h"
#include "version.h"

#ifdef WIN32
#include <windows.h>
#include "Win32/moviewin.h"
#endif

struct MovieType Movie;
struct MovieControlType MovieControl;

FILE* fpMovie = 0;
int iDoPauseAtVSync = 0; //if 1 call pause at next VSync
int iPause = 0;          //0: keep running emulation | 1: real pause
int iFrameAdvance = 0;   //1: advance one frame while key is down
int iGpuHasUpdated = 0;  //has the GPUchain function been called already?
int iVSyncFlag = 0;      //has a VSync already occured? (we can only save just after a VSync+iGpuHasUpdated)
int iJoysToPoll = 0;     //2: needs to poll both joypads | 1: only player 2 | 0: already polled both joypads for this frame

static const char szFileHeader[] = "PXM "; //movie file identifier

static void SetBytesPerFrame()
{
	Movie.bytesPerFrame = 1;
	switch (Movie.padType1) {
		case PSE_PAD_TYPE_STANDARD:
			Movie.bytesPerFrame += 2;
			break;
		case PSE_PAD_TYPE_MOUSE:
			Movie.bytesPerFrame += 4;
			break;
		case PSE_PAD_TYPE_ANALOGPAD:
		case PSE_PAD_TYPE_ANALOGJOY:
			Movie.bytesPerFrame += 6;
			break;
	}
	switch (Movie.padType2) {
		case PSE_PAD_TYPE_STANDARD:
			Movie.bytesPerFrame += 2;
			break;
		case PSE_PAD_TYPE_MOUSE:
			Movie.bytesPerFrame += 4;
			break;
		case PSE_PAD_TYPE_ANALOGPAD:
		case PSE_PAD_TYPE_ANALOGJOY:
			Movie.bytesPerFrame += 6;
			break;
	}
}

#define BUFFER_GROWTH_SIZE (4096)

static void ReserveInputBufferSpace(uint32 spaceNeeded)
{
	if (spaceNeeded > Movie.inputBufferSize) {
		uint32 ptrOffset = Movie.inputBufferPtr - Movie.inputBuffer;
		uint32 allocChunks = spaceNeeded / BUFFER_GROWTH_SIZE;
		Movie.inputBufferSize = BUFFER_GROWTH_SIZE * (allocChunks+1);
		Movie.inputBuffer = (uint8*)realloc(Movie.inputBuffer, Movie.inputBufferSize);
		Movie.inputBufferPtr = Movie.inputBuffer + ptrOffset;
	}
}


/*-----------------------------------------------------------------------------
-                              FILE OPERATIONS                                -
-----------------------------------------------------------------------------*/

int MOV_ReadMovieFile(char* szChoice, struct MovieType *tempMovie) {
	char readHeader[4];
	int empty;
	const char szFileHeader[] = "PXM ";       //file identifier
	FILE* fd;
	int nMetaLen;
	int i;
	int nCdidsLen;

	strncpy(tempMovie->movieFilename,szChoice,256);

	fd = fopen(tempMovie->movieFilename, "rb");
	if (!fd)
		return 0;

	fread(readHeader, 1, 4, fd);              //read identifier
	if (memcmp(readHeader,szFileHeader,4)) {  //not the right file type
		fclose(fd);
		return 0;
	}

	fread(&tempMovie->formatVersion,1,4,fd);  //file format version number
	if (tempMovie->formatVersion != MOVIE_VERSION) { //not the right version number
		fclose(fd);
		return 0;
	}
	fread(&tempMovie->emuVersion, 1, 4, fd);  //emulator version number

	fread(&tempMovie->movieFlags, 1, 1, fd);  //read flags
	{
		tempMovie->saveStateIncluded = tempMovie->movieFlags&MOVIE_FLAG_FROM_SAVESTATE;
		tempMovie->memoryCardIncluded = tempMovie->movieFlags&MOVIE_FLAG_MEMORY_CARDS;
		tempMovie->cheatListIncluded = tempMovie->movieFlags&MOVIE_FLAG_CHEAT_LIST;
		tempMovie->irqHacksIncluded = tempMovie->movieFlags&MOVIE_FLAG_IRQ_HACKS;
		tempMovie->palTiming = tempMovie->movieFlags&MOVIE_FLAG_PAL_TIMING;
	}
	fread(&empty, 1, 1, fd);  //reserved for more flags

	fread(&tempMovie->padType1, 1, 1, fd);
	fread(&tempMovie->padType2, 1, 1, fd);

	fread(&tempMovie->totalFrames, 1, 4, fd);
	fread(&tempMovie->rerecordCount, 1, 4, fd);
	fread(&tempMovie->saveStateOffset, 1, 4, fd);
	fread(&tempMovie->memoryCard1Offset, 1, 4, fd);
	fread(&tempMovie->memoryCard2Offset, 1, 4, fd);
	fread(&tempMovie->cheatListOffset, 1, 4, fd);
	fread(&tempMovie->cdIdsOffset, 1, 4, fd);
	fread(&tempMovie->inputOffset, 1, 4, fd);

	// read metadata
	fread(&nMetaLen, 1, 4, fd);

	if (nMetaLen >= MOVIE_MAX_METADATA)
		nMetaLen = MOVIE_MAX_METADATA-1;

//	tempMovie->authorInfo = (char*)malloc((nMetaLen+1)*sizeof(char));
	for(i=0; i<nMetaLen; ++i) {
		char c = 0;
		c |= fgetc(fd) & 0xff;
		tempMovie->authorInfo[i] = c;
	}
	tempMovie->authorInfo[i] = '\0';

	fseek(fd, tempMovie->cdIdsOffset, SEEK_SET);
	// read CDs IDs information
	fread(&tempMovie->CdromCount, 1, 1, fd);                 //total CDs used
	nCdidsLen = tempMovie->CdromCount*9;                 //CDs IDs
	for(i=0; i<nCdidsLen; ++i) {
		char c = 0;
		c |= fgetc(fd) & 0xff;
		tempMovie->CdromIds[i] = c;
	}

	// done reading file
	fclose(fd);

	return 1;
}

void MOV_WriteMovieFile()
{
	int cdidsLen;
	fseek(fpMovie, 12, SEEK_SET);
	fwrite(&Movie.movieFlags, 1, 1, fpMovie);    //flags
	fseek(fpMovie, 16, SEEK_SET);
	fwrite(&Movie.currentFrame, 1, 4, fpMovie);  //total frames
	fwrite(&Movie.rerecordCount, 1, 4, fpMovie); //rerecord count
	fseek(fpMovie, Movie.cdIdsOffset, SEEK_SET);
	fwrite(&Movie.CdromCount, 1, 1, fpMovie);    //total CDs used
	cdidsLen = Movie.CdromCount*9;
	if (cdidsLen > 0) {
		unsigned char* cdidsbuf = (unsigned char*)malloc(cdidsLen);
		int i;
		for(i=0; i<cdidsLen; ++i) {
			cdidsbuf[i + 0] = Movie.CdromIds[i] & 0xff;
		}
		fwrite(cdidsbuf, 1, cdidsLen, fpMovie);      //CDs IDs
		free(cdidsbuf);
	}
	fseek(fpMovie, 44, SEEK_SET);
	Movie.inputOffset = Movie.cdIdsOffset+1+(9*Movie.CdromCount);
	fwrite(&Movie.inputOffset, 1, 4, fpMovie);   //input offset
	Movie.totalFrames=Movie.currentFrame; //used when toggling read-only mode
	fseek(fpMovie, Movie.inputOffset, SEEK_SET);
	fwrite(Movie.inputBuffer, 1, Movie.bytesPerFrame*(Movie.totalFrames+1), fpMovie);
}

static void WriteMovieHeader()
{
	int empty=0;
	unsigned long emuVersion = PCSXRR_VERSION;
	unsigned long movieVersion = MOVIE_VERSION;
	int authLen;
	unsigned char* authbuf;
	int i;
	int cdidsLen;

	Movie.movieFlags=0;
	if (Movie.saveStateIncluded)
		Movie.movieFlags |= MOVIE_FLAG_FROM_SAVESTATE;
	if (Movie.memoryCardIncluded)
		Movie.movieFlags |= MOVIE_FLAG_MEMORY_CARDS;
	if (Movie.cheatListIncluded)
		Movie.movieFlags |= MOVIE_FLAG_CHEAT_LIST;
	if (Movie.irqHacksIncluded)
		Movie.movieFlags |= MOVIE_FLAG_IRQ_HACKS;
	if (Config.PsxType)
		Movie.movieFlags |= MOVIE_FLAG_PAL_TIMING;

	fwrite(&szFileHeader, 1, 4, fpMovie);          //header
	fwrite(&movieVersion, 1, 4, fpMovie);          //movie version
	fwrite(&emuVersion, 1, 4, fpMovie);            //emu version
	fwrite(&Movie.movieFlags, 1, 1, fpMovie);      //flags
	fwrite(&empty, 1, 1, fpMovie);                 //reserved for more flags
	fwrite(&Movie.padType1, 1, 1, fpMovie);        //padType1
	fwrite(&Movie.padType2, 1, 1, fpMovie);        //padType2
	fwrite(&empty, 1, 4, fpMovie);                 //total frames
	fwrite(&empty, 1, 4, fpMovie);                 //rerecord count
	fwrite(&empty, 1, 4, fpMovie);                 //savestate offset
	fwrite(&empty, 1, 4, fpMovie);                 //memory card 1 offset
	fwrite(&empty, 1, 4, fpMovie);                 //memory card 2 offset
	fwrite(&empty, 1, 4, fpMovie);                 //cheat list offset
	fwrite(&empty, 1, 4, fpMovie);                 //cdIds offset
	fwrite(&empty, 1, 4, fpMovie);                 //input offset

	authLen = strlen(Movie.authorInfo);
	if (authLen > 0) {
		fwrite(&authLen, 1, 4, fpMovie);             //author info size
		authbuf = (unsigned char*)malloc(authLen);
		for(i=0; i<authLen; ++i) {
			authbuf[i + 0] = Movie.authorInfo[i] & 0xff;
		}
		fwrite(authbuf, 1, authLen, fpMovie);        //author info
		free(authbuf);
	}

	Movie.saveStateOffset = ftell(fpMovie);        //get savestate offset
	if (!Movie.saveStateIncluded)
		fwrite(&empty, 1, 4, fpMovie);               //empty 4-byte savestate
	else {
		fclose(fpMovie);
		SaveStateEmbed(Movie.movieFilename);
		fpMovie = fopen(Movie.movieFilename,"r+b");
		fseek(fpMovie, 0, SEEK_END);
	}
	
	Movie.memoryCard1Offset = ftell(fpMovie);      //get memory card 1 offset
	if (!Movie.memoryCardIncluded) {
		fwrite(&empty, 1, 4, fpMovie);               //empty 4-byte memory card
		SIO_ClearMemoryCardsEmbed();
	}
	else {
		fclose(fpMovie);
		SIO_SaveMemoryCardsEmbed(Movie.movieFilename,1);
		fpMovie = fopen(Movie.movieFilename,"r+b");
		fseek(fpMovie, 0, SEEK_END);
	}
	Movie.memoryCard2Offset = ftell(fpMovie);      //get memory card 2 offset
	if (!Movie.memoryCardIncluded) {
		fwrite(&empty, 1, 4, fpMovie);               //empty 4-byte memory card
		SIO_ClearMemoryCardsEmbed();
	}
	else {
		fclose(fpMovie);
		SIO_SaveMemoryCardsEmbed(Movie.movieFilename,2);
		fpMovie = fopen(Movie.movieFilename,"r+b");
		fseek(fpMovie, 0, SEEK_END);
	}
	LoadMcds(Config.Mcd1, Config.Mcd2);

	Movie.cheatListOffset = ftell(fpMovie);        //get cheat list offset
	if (!Movie.cheatListIncluded) {
		fwrite(&empty, 1, 4, fpMovie);               //empty 4-byte cheat list
		CHT_ClearCheatFileEmbed();
	}
	else {
		fclose(fpMovie);
		CHT_SaveCheatFileEmbed(Movie.movieFilename);
		fpMovie = fopen(Movie.movieFilename,"r+b");
		fseek(fpMovie, 0, SEEK_END);
	}

	Movie.cdIdsOffset = ftell(fpMovie);            //get cdIds offset
	fwrite(&Movie.CdromCount, 1, 1, fpMovie);      //total CDs used
	cdidsLen = Movie.CdromCount*9;
	if (cdidsLen > 0) {
		unsigned char* cdidsbuf = (unsigned char*)malloc(cdidsLen);
		int i;
		for(i=0; i<cdidsLen; ++i) {
			cdidsbuf[i + 0] = Movie.CdromIds[i] & 0xff;
		}
		fwrite(cdidsbuf, 1, cdidsLen, fpMovie);      //CDs IDs
		free(cdidsbuf);
	}

	Movie.inputOffset = ftell(fpMovie);            //get input offset

	fseek (fpMovie, 24, SEEK_SET);
	fwrite(&Movie.saveStateOffset, 1, 4, fpMovie); //write savestate offset
	fwrite(&Movie.memoryCard1Offset, 1,4,fpMovie); //write memory card 1 offset
	fwrite(&Movie.memoryCard2Offset, 1,4,fpMovie); //write memory card 2 offset
	fwrite(&Movie.cheatListOffset, 1, 4, fpMovie); //write cheat list offset
	fwrite(&Movie.cdIdsOffset, 1, 4, fpMovie);     //write cd ids offset
	fwrite(&Movie.inputOffset, 1, 4, fpMovie);     //write input offset
	fseek(fpMovie, 0, SEEK_END);
}

static void TruncateMovie()
{
	long truncLen = Movie.inputOffset + Movie.bytesPerFrame*(Movie.totalFrames+1);

	HANDLE fileHandle = CreateFile(Movie.movieFilename, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, 0);
	if (fileHandle != NULL) {
		SetFilePointer(fileHandle, truncLen, 0, FILE_BEGIN);
		SetEndOfFile(fileHandle);
		CloseHandle(fileHandle);
	}
}

/*-----------------------------------------------------------------------------
-                            FILE OPERATIONS END                              -
-----------------------------------------------------------------------------*/


static int StartRecord()
{
	fpMovie = fopen(Movie.movieFilename,"w+b");
	SetBytesPerFrame();

	Movie.rerecordCount = 0;
	Movie.readOnly = 0;
	Movie.CdromCount = 1;
	sprintf(Movie.CdromIds, "%9.9s", CdromId);

	WriteMovieHeader();
	Movie.inputBufferPtr = Movie.inputBuffer;

	return 1;
}

static int StartReplay()
{
	uint32 toRead;
	SetBytesPerFrame();

	Config.PsxType = Movie.palTiming;

	if (Movie.saveStateIncluded)
		LoadStateEmbed(Movie.movieFilename);

	if (Movie.cheatListIncluded)
		CHT_LoadCheatFileEmbed(Movie.movieFilename);
	else
		CHT_ClearCheatFileEmbed();

	if (Movie.memoryCardIncluded)
		SIO_LoadMemoryCardsEmbed(Movie.movieFilename);
	else
		SIO_ClearMemoryCardsEmbed();

	// fill input buffer
	fpMovie = fopen(Movie.movieFilename,"r+b");
	{
		fseek(fpMovie, Movie.inputOffset, SEEK_SET);
		Movie.inputBufferPtr = Movie.inputBuffer;
		toRead = Movie.bytesPerFrame * (Movie.totalFrames+1);
		ReserveInputBufferSpace(toRead);
		fread(Movie.inputBufferPtr, 1, toRead, fpMovie);
	}

	return 1;
}

void MOV_StartMovie(int mode)
{
	Movie.mode = mode;
	Movie.currentFrame = 0;
	Movie.lagCounter = 0;
	Movie.currentCdrom = 1;
	cdOpenCase = 0;
	cheatsEnabled = 0;
	Config.Sio = 0;
	Config.SpuIrq = 0;
	Config.RCntFix = 0;
	Config.VSyncWA = 0;
	memset(&MovieControl, 0, sizeof(MovieControl));
	if (Movie.mode == MOVIEMODE_RECORD)
		StartRecord();
	else if (Movie.mode == MOVIEMODE_PLAY)
		StartReplay();
}

void MOV_StopMovie()
{
	if (Movie.mode == MOVIEMODE_RECORD) {
		MOV_WriteMovieFile();
		fclose(fpMovie);
		TruncateMovie();
	}
	else {
		fclose(fpMovie);
	}
	fpMovie = NULL;
	Movie.mode = MOVIEMODE_INACTIVE;
	SIO_UnsetTempMemoryCards();
}

#ifdef _MSC_VER_
#define inline __inline
#endif

inline static uint8 JoyRead8()
{
	uint8 v=Movie.inputBufferPtr[0];
	Movie.inputBufferPtr++;
	return v;
}
inline static uint16 JoyRead16()
{
	uint16 v=(Movie.inputBufferPtr[0] | (Movie.inputBufferPtr[1]<<8));
	Movie.inputBufferPtr += 2;
	return v;
}

void MOV_ReadJoy(PadDataS *pad,unsigned char type)
{
	switch (type) {
		case PSE_PAD_TYPE_MOUSE:
			pad->buttonStatus = JoyRead16();
			pad->moveX = JoyRead8();
			pad->moveY = JoyRead8();
			break;
		case PSE_PAD_TYPE_ANALOGPAD: // scph1150
			pad->buttonStatus = JoyRead16();
			pad->leftJoyX = JoyRead8();
			pad->leftJoyY = JoyRead8();
			pad->rightJoyX = JoyRead8();
			pad->rightJoyY = JoyRead8();
			break;
		case PSE_PAD_TYPE_ANALOGJOY: // scph1110
			pad->buttonStatus = JoyRead16();
			pad->leftJoyX = JoyRead8();
			pad->leftJoyY = JoyRead8();
			pad->rightJoyX = JoyRead8();
			pad->rightJoyY = JoyRead8();
			break;
		case PSE_PAD_TYPE_STANDARD:
		default:
			pad->buttonStatus = JoyRead16();
	}
	pad->buttonStatus ^= 0xffff;
	pad->controllerType = type;
}

static void JoyWrite8(uint8 v)
{
	Movie.inputBufferPtr[0]=(uint8)v;
	Movie.inputBufferPtr += 1;
}
static void JoyWrite16(uint16 v)
{
	Movie.inputBufferPtr[0]=(uint8)v;
	Movie.inputBufferPtr[1]=(uint8)(v>>8);
	Movie.inputBufferPtr += 2;
}

void MOV_WriteJoy(PadDataS *pad,unsigned char type)
{
	switch (type) {
		case PSE_PAD_TYPE_MOUSE:
			ReserveInputBufferSpace((uint32)((Movie.inputBufferPtr+4)-Movie.inputBuffer));
			JoyWrite16(pad->buttonStatus^0xFFFF);
			JoyWrite8(pad->moveX);
			JoyWrite8(pad->moveY);
			break;
		case PSE_PAD_TYPE_ANALOGPAD: // scph1150
			ReserveInputBufferSpace((uint32)((Movie.inputBufferPtr+6)-Movie.inputBuffer));
			JoyWrite16(pad->buttonStatus^0xFFFF);
			JoyWrite8(pad->leftJoyX);
			JoyWrite8(pad->leftJoyY);
			JoyWrite8(pad->rightJoyX);
			JoyWrite8(pad->rightJoyY);
			break;
		case PSE_PAD_TYPE_ANALOGJOY: // scph1110
			ReserveInputBufferSpace((uint32)((Movie.inputBufferPtr+6)-Movie.inputBuffer));
			JoyWrite16(pad->buttonStatus^0xFFFF);
			JoyWrite8(pad->leftJoyX);
			JoyWrite8(pad->leftJoyY);
			JoyWrite8(pad->rightJoyX);
			JoyWrite8(pad->rightJoyY);
			break;
		case PSE_PAD_TYPE_STANDARD:
		default:
			ReserveInputBufferSpace((uint32)((Movie.inputBufferPtr+2)-Movie.inputBuffer));
			JoyWrite16(pad->buttonStatus^0xFFFF);
	}
}

void MOV_ReadControl() {
	char controlFlags = JoyRead8();

	MovieControl.reset = controlFlags&MOVIE_CONTROL_RESET;
	MovieControl.cdCase = controlFlags&MOVIE_CONTROL_CDCASE;
	MovieControl.sioIrq = controlFlags&MOVIE_CONTROL_SIOIRQ;
	MovieControl.spuIrq = controlFlags&MOVIE_CONTROL_SPUIRQ;
	MovieControl.cheats = controlFlags&MOVIE_CONTROL_CHEATS;
	MovieControl.RCntFix = controlFlags&MOVIE_CONTROL_RCNTFIX;
	MovieControl.VSyncWA = controlFlags&MOVIE_CONTROL_VSYNCWA;
}

void MOV_WriteControl() {
	char controlFlags = 0;

	if (MovieControl.reset)
		controlFlags |= MOVIE_CONTROL_RESET;
	if (MovieControl.cdCase)
		controlFlags |= MOVIE_CONTROL_CDCASE;
	if (MovieControl.sioIrq)
		controlFlags |= MOVIE_CONTROL_SIOIRQ;
	if (MovieControl.spuIrq)
		controlFlags |= MOVIE_CONTROL_SPUIRQ;
	if (MovieControl.cheats)
		controlFlags |= MOVIE_CONTROL_CHEATS;
	if (MovieControl.RCntFix)
		controlFlags |= MOVIE_CONTROL_RCNTFIX;
	if (MovieControl.VSyncWA)
		controlFlags |= MOVIE_CONTROL_VSYNCWA;

	ReserveInputBufferSpace((uint32)((Movie.inputBufferPtr+1)-Movie.inputBuffer));
	JoyWrite8(controlFlags);
}

void MOV_ProcessControlFlags() {
	if (MovieControl.cdCase) {
		cdOpenCase ^= -1;
		if (cdOpenCase < 0) {
			Movie.currentCdrom++;
			Movie.CdromCount++;
			#ifdef WIN32
				MOV_W32_CdChangeDialog();
			#else
				//TODO: pause and ask for a new CD?
			#endif
		}
		else {
			CDR_close();
			CDR_open();
			CheckCdrom();
			if (LoadCdrom() == -1)
				SysMessage(_("Could not load Cdrom"));
			sprintf(Movie.CdromIds, "%s%9.9s", Movie.CdromIds,CdromId);
		}
	}
	if (MovieControl.cheats)
		cheatsEnabled ^= 1;
	if (MovieControl.reset) {
		MovieControl.reset = 0;
		LoadCdBios = 0;
		SysReset();
		CheckCdrom();
		if (LoadCdrom() == -1)
			SysMessage(_("Could not load Cdrom"));
		psxCpu->Execute();
	}
	if ((MovieControl.spuIrq || MovieControl.sioIrq ||
		   MovieControl.RCntFix || MovieControl.VSyncWA) &&
		   !Movie.irqHacksIncluded && Movie.mode == MOVIEMODE_RECORD) {
		Movie.irqHacksIncluded = 1;
		Movie.movieFlags |= MOVIE_FLAG_IRQ_HACKS;
	}
	if (MovieControl.sioIrq && Movie.irqHacksIncluded)
		Config.Sio ^= 1;
	if (MovieControl.spuIrq && Movie.irqHacksIncluded)
		Config.SpuIrq ^= 1;
	if (MovieControl.RCntFix && Movie.irqHacksIncluded)
		Config.RCntFix ^= 1;
	if (MovieControl.VSyncWA && Movie.irqHacksIncluded)
		Config.VSyncWA ^= 1;

	memset(&MovieControl, 0, sizeof(MovieControl));
}

int MovieFreeze(gzFile f, int Mode) {
	unsigned long bufSize = 0;
	unsigned long buttonToSend = 0;

	//saving state
	if (Mode == 1)
		bufSize = Movie.bytesPerFrame * (Movie.currentFrame+1);

	//saving/loading state
	gzfreezel(&Movie.currentFrame);
	gzfreezel(&Movie.lagCounter);
	if (Mode == 0) {
		//update information GPU OSD after loading a savestate
		GPU_setlagcounter(Movie.lagCounter);
		GPU_setframecounter(Movie.currentFrame,Movie.totalFrames);
	}

	if (Movie.mode == MOVIEMODE_INACTIVE)
		return 0;

	//saving/loading state
	gzfreezel(&cdOpenCase);
	gzfreezel(&cheatsEnabled);
	gzfreezel(&Movie.irqHacksIncluded);
	gzfreezel(&Config.Sio);
	gzfreezel(&Config.SpuIrq);
	gzfreezel(&Config.RCntFix);
	gzfreezel(&Config.VSyncWA);
	gzfreezel(&Movie.lastPad1);
	gzfreezel(&Movie.lastPad2);
	gzfreezel(&Movie.currentCdrom);
	gzfreezel(&Movie.CdromCount);
	gzfreeze(Movie.CdromIds,Movie.CdromCount*9);
	gzfreezel(&bufSize);
	if (!(Movie.mode == MOVIEMODE_PLAY && Mode == 0)) {
		gzfreeze(Movie.inputBuffer, bufSize);
	}

	//loading state
	if (Mode == 0) {
		if (Movie.mode == MOVIEMODE_RECORD && !PCSX_LuaRerecordCountSkip())
			Movie.rerecordCount++;
		Movie.inputBufferPtr = Movie.inputBuffer+(Movie.bytesPerFrame * Movie.currentFrame);
		
		//update information GPU OSD after loading a savestate
		buttonToSend = Movie.lastPad1.buttonStatus;
		buttonToSend = (buttonToSend ^ (Movie.lastPad2.buttonStatus << 16));
		GPU_inputdisplay(buttonToSend);
	}

	return 0;
}

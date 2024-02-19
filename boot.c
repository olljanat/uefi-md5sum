/*
 * uefi-md5sum: UEFI MD5Sum validator
 * Copyright © 2023-2024 Pete Batard <pete@akeo.ie>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "boot.h"

/* The following header is generated by the build process on prod */
#include "version.h"

/*
 * When performing tests with GitHub Actions, we want to remove all
 * colour formatting as well force shutdown on exit (to exit qemu)
 * so we need a variable to tell us if we are running in test mode.
 */
BOOLEAN IsTestMode = FALSE;

/* Incremental vertical position at which we display alert messages */
UINTN AlertYPos = ROWS_MIN / 2 + 1;

/* We'll use this string to erase lines on the console */
STATIC CHAR16 EmptyLine[STRING_MAX] = { 0 };

/* Keep a copy of the main Image Handle */
STATIC EFI_HANDLE MainImageHandle = NULL;

/* We'll need to reference the dimensions of the UEFI text console */
STATIC struct {
	UINTN Cols;
	UINTN Rows;
} Console = { COLS_MIN, ROWS_MIN };

/* Strings used for platform identification */
#if defined(_M_X64) || defined(__x86_64__)
STATIC CHAR16* Arch = L"x64";
#elif defined(_M_IX86) || defined(__i386__)
STATIC CHAR16* Arch = L"ia32";
#elif defined (_M_ARM64) || defined(__aarch64__)
STATIC CHAR16* Arch = L"aa64";
#elif defined (_M_ARM) || defined(__arm__)
STATIC CHAR16* Arch = L"arm";
#elif defined(_M_RISCV64) || (defined (__riscv) && (__riscv_xlen == 64))
STATIC CHAR16* Arch = L"riscv64";
#else
#error Unsupported architecture
#endif

/* Variables used for progress tracking */
STATIC UINTN ProgressLastCol = 0;
STATIC BOOLEAN ProgressInit = FALSE;
STATIC UINTN ProgressYPos = ROWS_MIN / 2;
STATIC UINTN ProgressPPos = 0;

/**
  Print a centered message on the console.

  @param[in]  Message    The text message to print.
  @param[in]  YPos       The vertical position to print the message to.
**/
STATIC VOID PrintCentered(
	IN CHAR16* Message,
	IN UINTN YPos
)
{
	UINTN MessagePos;

	if (!IsTestMode) {
		MessagePos = Console.Cols / 2 - SafeStrLen(Message) / 2;
		V_ASSERT(MessagePos > MARGIN_H);
		SetTextPosition(0, YPos);
		Print(EmptyLine);
		SetTextPosition(MessagePos, YPos);
	}
	Print(L"%s\n", Message);
}

/**
  Print a hash entry that has failed processing.
  Do this over a specific section of the console we cycle over.

  @param[in]  Status     The Status code from the failed operation on the entry.
  @param[in]  Path       A pointer to the CHAR16 string with the Path of the entry.
  @param[in]  NumFailed  The current number of failed entries.
**/
STATIC VOID PrintFailedEntry(
	IN CONST EFI_STATUS Status,
	IN CHAR16* Path,
	IN CONST UINTN NumFailed
)
{
	if (!EFI_ERROR(Status) || Path == NULL)
		return;

	// Truncate the path in case it's very long.
	// TODO: Ideally we'd want long path reduction similar to what Windows does.
	if (SafeStrLen(Path) > 80)
		Path[80] = L'\0';
	SetTextPosition(MARGIN_H, 1 + (NumFailed % (Console.Rows / 2 - 4)));
	if (!IsTestMode) {
		gST->ConOut->OutputString(gST->ConOut, EmptyLine);
		SetTextPosition(MARGIN_H, 1 + (NumFailed % (Console.Rows / 2 - 4)));
	}
	SetText(TEXT_RED);
	Print(L"[FAIL]");
	DefText();
	// Display a more explicit message (than "CRC Error") for files that fail MD5
	if ((Status & 0x7FFFFFFF) == 27)
		Print(L" File '%s': [27] MD5 Checksum Error\n", Path);
	else
		Print(L" File '%s': [%d] %r\n", Path, (Status & 0x7FFFFFFF), Status);
}

/**
  Display a countdown on screen.

  @param[in]  Message   A message to display with the countdown.
  @param[in]  Duration  The duration of the countdown (in ms).
**/
STATIC VOID CountDown(
	IN CHAR16* Message,
	IN UINTN Duration
)
{
	UINTN MessagePos, CounterPos;
	INTN i;

	if (IsTestMode)
		return;

	MessagePos = Console.Cols / 2 - SafeStrLen(Message) / 2 - 1;
	CounterPos = MessagePos + SafeStrLen(Message) + 2;
	V_ASSERT(MessagePos > MARGIN_H);
	SetTextPosition(0, Console.Rows - 2);
	Print(EmptyLine);
	SetTextPosition(MessagePos, Console.Rows - 2);
	SetText(TEXT_YELLOW);
	Print(L"[%s ", Message);

	gST->ConIn->Reset(gST->ConIn, FALSE);
	for (i = (INTN)Duration; i >= 0; i -= 200) {
		// Allow the user to press a key to interrupt the countdown
		if (gST->BootServices->CheckEvent(gST->ConIn->WaitForKey) != EFI_NOT_READY)
			break;
		if (i % 1000 == 0) {
			SetTextPosition(CounterPos, Console.Rows - 2);
			Print(L"%d]   ", i / 1000);
		}
		Sleep(200000);
	}
}

/**
  Process exit according to the multiple scenarios we want to handle
  (Chain load the next bootloader, shutdown if test mode, etc.).

  @param[in]  Status      The current EFI Status of the application.
  @param[in]  DevicePath  (Optional) Device Path of a bootloader to chain load.
**/
STATIC EFI_STATUS ExitProcess(
	IN EFI_STATUS Status,
	IN EFI_DEVICE_PATH* DevicePath
)
{
	UINTN Index;
	EFI_HANDLE ImageHandle;
	EFI_INPUT_KEY Key;
	BOOLEAN RunCountDown = TRUE;

	// If we have a bootloader to chain load, try to launch it
	if (DevicePath != NULL) {
		if (EFI_ERROR(Status) && !IsTestMode) {
			// Ask the user if they want to continue
			SetText(TEXT_YELLOW);
			PrintCentered(L"Proceed with boot? [y/N]", Console.Rows - 2);
			gST->ConIn->Reset(gST->ConIn, FALSE);
			while (gST->ConIn->ReadKeyStroke(gST->ConIn, &Key) == EFI_NOT_READY);
			if (Key.UnicodeChar != L'y' && Key.UnicodeChar != L'Y') {
				SafeFree(DevicePath);
				return Status;
			}
			RunCountDown = FALSE;
		}
		Status = gBS->LoadImage(FALSE, MainImageHandle, DevicePath, NULL, 0, &ImageHandle);
		SafeFree(DevicePath);
		if (Status == EFI_SUCCESS) {
			if (RunCountDown)
				CountDown(L"Launching next bootloader in", 3000);
			if (!IsTestMode)
				gST->ConOut->ClearScreen(gST->ConOut);
			Status = gBS->StartImage(ImageHandle, NULL, NULL);
		}
		if (EFI_ERROR(Status)) {
			SetTextPosition(MARGIN_H, Console.Rows / 2 + 1);
			PrintError(L"Could not launch original bootloader");
		}
	}

	// If running in test mode, shut down QEMU
	if (IsTestMode)
		ShutDown();

	// Wait for a user keystroke as needed
#if !defined(EFI_DEBUG)
	if (EFI_ERROR(Status)) {
#endif
		SetText(TEXT_YELLOW);
		PrintCentered(L"[Press any key to exit]", Console.Rows - 2);
		DefText();
		gST->ConIn->Reset(gST->ConIn, FALSE);
		gST->BootServices->WaitForEvent(1, &gST->ConIn->WaitForKey, &Index);
#if defined(EFI_DEBUG)
		ShutDown();
# else
	}
#endif
	return Status;
}

/**
  Obtain the device and root handle of the current volume.

  @param[out] DeviceHandle      A pointer to the device handle.
  @param[out] Root              A pointer to the root file handle.

  @retval EFI_SUCCESS           The root file handle was successfully populated.
  @retval EFI_INVALID_PARAMETER The pointer to the root file handle is invalid.
**/
STATIC EFI_STATUS GetRootHandle(
	OUT EFI_HANDLE* DeviceHandle,
	OUT EFI_FILE_HANDLE* Root
)
{
	EFI_STATUS Status;
	EFI_LOADED_IMAGE_PROTOCOL* LoadedImage;
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* Volume;

	if (DeviceHandle == NULL || Root == NULL)
		return EFI_INVALID_PARAMETER;

	// Access the loaded image so we can open the current volume
	Status = gBS->OpenProtocol(MainImageHandle, &gEfiLoadedImageProtocolGuid,
		(VOID**)&LoadedImage, MainImageHandle,
		NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
	if (EFI_ERROR(Status))
		return Status;
	*DeviceHandle = LoadedImage->DeviceHandle;

	// Open the the root directory on the boot volume
	Status = gBS->OpenProtocol(LoadedImage->DeviceHandle,
		&gEfiSimpleFileSystemProtocolGuid, (VOID**)&Volume,
		MainImageHandle, NULL, EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
	if (EFI_ERROR(Status))
		return Status;

	return Volume->OpenVolume(Volume, Root);
}

/**
  Initialize the progress bar.

  @param[in]  Message    The text message to print above the progress bar.
  @param[in]  YPos       The vertical position the progress bar should be displayed.
**/
STATIC VOID InitProgress(
	IN CHAR16* Message,
	IN UINTN YPos
)
{
	UINTN i, MessagePos;

	ProgressInit = FALSE;


	if (Console.Cols < COLS_MIN || Console.Rows < ROWS_MIN ||
		Console.Cols >= STRING_MAX || IsTestMode)
		return;

	if (SafeStrLen(Message) > Console.Cols - MARGIN_H * 2 - 8)
		return;

	if (YPos > Console.Rows - 3)
		YPos = Console.Rows - 3;

	MessagePos = Console.Cols / 2 - (SafeStrLen(Message) + 6) / 2;
	V_ASSERT(MessagePos > MARGIN_H);

	ProgressLastCol = 0;
	ProgressYPos = YPos;
	ProgressPPos = MessagePos + SafeStrLen(Message) + 2;

	SetTextPosition(MessagePos, ProgressYPos);
	Print(L"%s: 0.0%%", Message);

	SetTextPosition(MARGIN_H, ProgressYPos + 1);
	for (i = 0; i < Console.Cols - MARGIN_H * 2; i++)
		Print(L"░");

	ProgressInit = TRUE;
}

/**
  Update the progress bar.

  @param[in]  CurValue   Updated current value within the progress bar.
  @param[in]  MaxValue   Value at which the progress bar should display 100%.
**/
STATIC VOID PrintProgress(
	IN UINT64 CurValue,
	IN UINT64 MaxValue
)
{
	UINTN CurCol, PerMille;

	if (Console.Cols < COLS_MIN || Console.Cols >= STRING_MAX || IsTestMode || !ProgressInit)
		return;

	if (CurValue > MaxValue)
		CurValue = MaxValue;

	// Update the percentage figure
	PerMille = (UINTN)((CurValue * 1000) / MaxValue);
	SetTextPosition(ProgressPPos, ProgressYPos);
	Print(L"%d.%d%%", PerMille / 10, PerMille % 10);

	// Update the progress bar
	CurCol = (UINTN)((CurValue * (Console.Cols - MARGIN_H * 2)) / MaxValue);
	for (; CurCol > ProgressLastCol && ProgressLastCol < Console.Cols; ProgressLastCol++) {
		SetTextPosition(MARGIN_H + ProgressLastCol, ProgressYPos + 1);
		Print(L"%c", BLOCKELEMENT_FULL_BLOCK);
	}

	if (CurValue == MaxValue)
		ProgressInit = FALSE;
}

/*
 * Application entry-point
 * NB: This must be set to 'efi_main' for gnu-efi crt0 compatibility
 */
EFI_STATUS EFIAPI efi_main(
	IN EFI_HANDLE BaseImageHandle,
	IN EFI_SYSTEM_TABLE* SystemTable
)
{
	EFI_STATUS Status;
	EFI_HANDLE DeviceHandle;
	EFI_FILE_HANDLE Root;
	EFI_DEVICE_PATH* DevicePath = NULL;
	EFI_INPUT_KEY Key;
	HASH_LIST HashList = { 0 };
	CHAR8 c;
	CHAR16 Path[PATH_MAX + 1], Message[128], LoaderPath[64];
	UINT8 ComputedHash[MD5_HASHSIZE], ExpectedHash[MD5_HASHSIZE];
	UINTN i, Index, NumFailed = 0;

	// Keep a global copy of the bootloader's image handle
	MainImageHandle = BaseImageHandle;

#if defined(_GNU_EFI)
	InitializeLib(BaseImageHandle, SystemTable);
#endif

	// Determine if we are running in test mode.
	// Note that test mode is no less secure than regular mode.
	// It only produces or removes extra onscreen output.
	IsTestMode = IsTestSystem();

	// Clear the console
	if (!IsTestMode)
		gST->ConOut->ClearScreen(gST->ConOut);

	// Find the amount of console real-estate we have at out disposal
	Status = gST->ConOut->QueryMode(gST->ConOut, gST->ConOut->Mode->Mode,
		&Console.Cols, &Console.Rows);
	if (EFI_ERROR(Status)) {
		// Couldn't get the console dimensions
		Console.Cols = COLS_MIN;
		Console.Rows = ROWS_MIN;
	}
	if (Console.Cols >= STRING_MAX)
		Console.Cols = STRING_MAX - 1;
	AlertYPos = Console.Rows / 2 + 1;

	// Populate a blank line we can use to erase a line
	for (i = 0; i < Console.Cols; i++)
		EmptyLine[i] = L' ';
	EmptyLine[i] = L'\0';

	// Print the reference URL for this application
	SetText(EFI_TEXT_ATTR(EFI_DARKGRAY, EFI_BLACK));
	PrintCentered(L"https://md5.akeo.ie", 0);
	DefText();

	Status = GetRootHandle(&DeviceHandle, &Root);
	if (EFI_ERROR(Status)) {
		PrintError(L"Could not open root directory\n");
		goto out;
	}

	// Look up the original boot loader for chain loading
	UnicodeSPrint(LoaderPath, ARRAY_SIZE(LoaderPath),
		L"\\efi\\boot\\boot%s_original.efi", Arch);
	if (SetPathCase(Root, LoaderPath) == EFI_SUCCESS)
		DevicePath = FileDevicePath(DeviceHandle, LoaderPath);

	// Parse md5sum.txt to construct a hash list
	Status = Parse(Root, HASH_FILE, &HashList);
	if (EFI_ERROR(Status))
		goto out;

	if (IsTestMode) {
		// Print any extra data we want to validate
		Print(L"[TEST] TotalBytes = 0x%lX\n", HashList.TotalBytes);
	}

	InitProgress(L"Media verification", Console.Rows / 2 - 3);
	SetText(TEXT_YELLOW);
	if (!IsTestMode)
		PrintCentered(L"[Press any key to cancel]", Console.Rows - 2);
	DefText();

	// Now go through each entry we parsed
	for (Index = 0; Index < HashList.NumEntries; Index++) {
		// Check for user cancellation
		if (gST->ConIn->ReadKeyStroke(gST->ConIn, &Key) != EFI_NOT_READY)
			break;

		// Report progress
		PrintProgress(Index, HashList.NumEntries);

		// Convert the expected hexascii hash to a binary value we can use
		ZeroMem(ExpectedHash, sizeof(ExpectedHash));
		for (i = 0; i < MD5_HASHSIZE * 2; i++) {
			c = HashList.Entry[Index].Hash[i];
			// The Parse() call should have filtered any invalid string
			V_ASSERT(IsValidHexAscii(c));
			ExpectedHash[i / 2] <<= 4;
			ExpectedHash[i / 2] |= c >= 'a' ? (c - 'a' + 0x0A) : c - '0';
		}

		// Convert the UTF-8 path to UCS-2
		Status = Utf8ToUcs2(HashList.Entry[Index].Path, Path, ARRAY_SIZE(Path));
		if (EFI_ERROR(Status)) {
			// Conversion failed but we want a UCS-2 Path for the failure
			// report so just filter out anything that is non lower ASCII.
			V_ASSERT(AsciiStrLen(HashList.Entry[Index].Path) < ARRAY_SIZE(Path));
			for (i = 0; i < AsciiStrLen(HashList.Entry[Index].Path); i++) {
				c = HashList.Entry[Index].Path[i];
				if (c < ' ' || c > 0x80)
					c = '?';
				Path[i] = (CHAR16)c;
			}
			Path[i] = L'\0';
		} else {
			// Hash the file and compare the result to the expected value
			// TODO: We should also handle progress & cancellation in HashFile()
			Status = HashFile(Root, Path, ComputedHash);
			if (Status == EFI_SUCCESS &&
				(CompareMem(ComputedHash, ExpectedHash, MD5_HASHSIZE) != 0))
				Status = EFI_CRC_ERROR;
		}

		// Report failures
		if (EFI_ERROR(Status))
			PrintFailedEntry(Status, Path, NumFailed++);
	}

	// Final progress report
	PrintProgress(Index, HashList.NumEntries);
	UnicodeSPrint(Message, ARRAY_SIZE(Message), L"%d/%d file%s processed",
		Index, HashList.NumEntries, (HashList.NumEntries == 1) ? L"" : L"s");
	V_ASSERT(SafeStrLen(Message) < ARRAY_SIZE(Message) / 2);
	UnicodeSPrint(&Message[SafeStrLen(Message)], ARRAY_SIZE(Message) - SafeStrLen(Message),
		L" [%d failed]", NumFailed);
	PrintCentered(Message, ProgressYPos + 2);

out:
	SafeFree(HashList.Buffer);
	if (Status == EFI_SUCCESS && NumFailed != 0)
		Status = EFI_CRC_ERROR;
	return ExitProcess(Status, DevicePath);
}

/*
 * uefi-md5sum: UEFI MD5Sum validator
 * Copyright © 2023 Pete Batard <pete@akeo.ie>
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

/* Copies of the global image handle and system table for the current executable */
EFI_SYSTEM_TABLE*   MainSystemTable = NULL;
EFI_HANDLE          MainImageHandle = NULL;

/* Strings used to identify the plaform */
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
#  error Unsupported architecture
#endif

/**
  Display a centered application banner
 **/
STATIC VOID DisplayBanner(VOID)
{
	UINTN i, Len;
	CHAR16 String[BANNER_LINE_SIZE + 1];

	// The platform logo may still be displayed → remove it
	gST->ConOut->ClearScreen(gST->ConOut);

	SetText(TEXT_REVERSED);
	Print(L"\n%c", BOXDRAW_DOWN_RIGHT);
	for (i = 0; i < BANNER_LINE_SIZE - 2; i++)
		Print(L"%c", BOXDRAW_HORIZONTAL);
	Print(L"%c\n", BOXDRAW_DOWN_LEFT);

	UnicodeSPrint(String, ARRAY_SIZE(String), L"UEFI md5sum %s (%s)", VERSION_STRING, Arch);
	Len = SafeStrLen(String);
	V_ASSERT(Len < BANNER_LINE_SIZE);
	Print(L"%c", BOXDRAW_VERTICAL);
	for (i = 1; i < (BANNER_LINE_SIZE - Len) / 2; i++)
		Print(L" ");
	Print(String);
	for (i += Len; i < BANNER_LINE_SIZE - 1; i++)
		Print(L" ");
	Print(L"%c\n", BOXDRAW_VERTICAL);

	UnicodeSPrint(String, ARRAY_SIZE(String), L"<https://md5.akeo.ie>");
	Len = SafeStrLen(String);
	V_ASSERT(Len < BANNER_LINE_SIZE);
	Print(L"%c", BOXDRAW_VERTICAL);
	for (i = 1; i < (BANNER_LINE_SIZE - Len) / 2; i++)
		Print(L" ");
	Print(String);
	for (i += Len; i < BANNER_LINE_SIZE - 1; i++)
		Print(L" ");
	Print(L"%c\n", BOXDRAW_VERTICAL);

	Print(L"%c", BOXDRAW_UP_RIGHT);
	for (i = 0; i < 77; i++)
		Print(L"%c", BOXDRAW_HORIZONTAL);
	Print(L"%c\n\n", BOXDRAW_UP_LEFT);
	DefText();
}

/*
 * Application entry-point
 * NB: This must be set to 'efi_main' for gnu-efi crt0 compatibility
 */
EFI_STATUS EFIAPI efi_main(EFI_HANDLE BaseImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
	EFI_STATUS Status = EFI_SUCCESS;
	EFI_LOADED_IMAGE_PROTOCOL* LoadedImage;
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* Volume;
	EFI_FILE_HANDLE Root;
	HASH_LIST HashList = { 0 };
	INTN SecureBootStatus;
#if defined(EFI_DEBUG)
	UINTN Event;
#endif

	MainSystemTable = SystemTable;
	MainImageHandle = BaseImageHandle;

#if defined(_GNU_EFI)
	InitializeLib(BaseImageHandle, SystemTable);
#endif

	IsTestMode = IsTestSystem();

	// Display non-critical system information
	DisplayBanner();
	PrintSystemInfo();
	SecureBootStatus = GetSecureBootStatus();
	SetText(TEXT_WHITE);
	Print(L"[INFO]");
	DefText();
	Print(L" Secure Boot status: ");
	if (SecureBootStatus == 0) {
		Print(L"Disabled\n");
	} else {
		SetText((SecureBootStatus > 0) ? TEXT_WHITE : TEXT_YELLOW);
		Print(L"%s\n", (SecureBootStatus > 0) ? L"Enabled" : L"Setup");
		DefText();
	}

	Status = gBS->OpenProtocol(MainImageHandle, &gEfiLoadedImageProtocolGuid,
		(VOID**)&LoadedImage, MainImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
	if (EFI_ERROR(Status)) {
		PrintError(L"Unable to access boot image interface");
		goto out;
	}

	// Open the the root directory on the boot volume
	Status = gBS->OpenProtocol(LoadedImage->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid,
		(VOID**)&Volume, MainImageHandle, NULL, EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
	if (EFI_ERROR(Status)) {
		PrintError(L"Unable to open boot volume");
		goto out;
	}
	Root = NULL;
	Status = Volume->OpenVolume(Volume, &Root);
	if (EFI_ERROR(Status)) {
		PrintError(L"Unable to open root directory");
		goto out;
	}

	Status = Parse(Root, HASH_FILE, &HashList);
	if (EFI_ERROR(Status))
		goto out;

	Print(L"Found %d entries (Total Bytes = 0x%lX)\n", HashList.Size, HashList.TotalBytes);

out:
	// If running in test mode, close QEMU by invoking shutdown
	if (IsTestMode)
		SHUTDOWN;

#if defined(EFI_DEBUG)
	// If running debug, wait for a user keystroke and shutdown
	SetText(TEXT_YELLOW);
	Print(L"\nPress any key to exit.\n");
	DefText();
	gST->ConIn->Reset(gST->ConIn, FALSE);
	gST->BootServices->WaitForEvent(1, &gST->ConIn->WaitForKey, &Event);
	SHUTDOWN;
#endif

	return Status;
}

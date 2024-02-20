' Visual Studio QEMU debugging script.
'
' I like invoking vbs as much as anyone else, but we need to download and unzip a
' bunch of files, as well as launch QEMU, and neither Powershell or a standard batch
' can do that without having an extra console appearing.
'
' Note: You may get a prompt from the firewall when trying to download files

' Modify these variables as needed
QEMU_PATH  = "C:\Program Files\qemu\"
' You can add something like "-S -gdb tcp:127.0.0.1:1234" if you plan to use gdb to debug
QEMU_OPTS  = "-nodefaults -vga std -serial vc"
' Set to True if you need to download a file that might be cached locally
NO_CACHE   = False

' You shouldn't have to mofify anything below this
CONF       = WScript.Arguments(0)
BIN        = WScript.Arguments(1)
TARGET     = WScript.Arguments(2)

If (TARGET = "x86") Then
  UEFI_EXT  = "ia32"
  QEMU_ARCH = "i386"
  FW_BASE   = "OVMF"
ElseIf (TARGET = "x64") Then
  UEFI_EXT  = "x64"
  QEMU_ARCH = "x86_64"
  FW_BASE   = "OVMF"
ElseIf (TARGET = "ARM") Then
  UEFI_EXT  = "arm"
  QEMU_ARCH = "arm"
  FW_BASE   = "AAVMF"
  ' You can also add '-device VGA' to the options below, to get graphics output.
  ' But if you do, be mindful that the keyboard input may not work... :(
  QEMU_OPTS = "-M virt -cpu cortex-a15 " & QEMU_OPTS
ElseIf (TARGET = "ARM64") Then
  UEFI_EXT  = "aa64"
  QEMU_ARCH = "aarch64"
  FW_BASE   = "AAVMF"
  QEMU_OPTS = "-M virt -cpu cortex-a57 " & QEMU_OPTS
Else
  MsgBox("Unsupported debug target: " & TARGET)
  Call WScript.Quit(1)
End If
BOOT_NAME  = "boot" & UEFI_EXT & ".efi"
FW_ARCH    = UCase(UEFI_EXT)
FW_DIR     = "https://efi.akeo.ie/" & FW_BASE & "/"
FW_ZIP     = FW_BASE & "-" & FW_ARCH & ".zip"
FW_FILE    = FW_BASE & "_" & FW_ARCH & ".fd"
FW_URL     = FW_DIR & FW_ZIP
QEMU_EXE   = "qemu-system-" & QEMU_ARCH & "w.exe"

' Globals
Set fso = CreateObject("Scripting.FileSystemObject")
Set shell = CreateObject("WScript.Shell")

' Follow through an HTTP redirect and return the target URL
Function GetRedirect(Url)
  Const WHR_EnableRedirects = 6
  Set oHttp = CreateObject("WinHttp.WinHttpRequest.5.1")
  oHttp.Option(WHR_EnableRedirects) = False
  oHttp.Open "HEAD", Url, False
  oHttp.Send
  If Err.Number = 0 Then
    If oHttp.Status = 200 Then
      GetRedirect = Url
    ElseIf oHttp.Status = 301 Or oHttp.Status = 302 Or oHttp.Status = 303 Then
      GetRedirect = oHttp.getResponseHeader("Location")
    End If
  Else
    GetRedirect = "Error " & Err.Number & ": " & Err.Source & " " & Err.Description
  End If
End Function

' Download a file from HTTP
Sub DownloadHttp(Url, File)
  Const BINARY = 1
  Const OVERWRITE = 2
  Set xHttp = CreateObject("Microsoft.XMLHTTP")
  Set bStrm = CreateObject("Adodb.Stream")
  Call xHttp.Open("GET", Url, False)
  If NO_CACHE = True Then
    Call xHttp.SetRequestHeader("If-None-Match", "some-random-string")
    Call xHttp.SetRequestHeader("Cache-Control", "no-cache,max-age=0")
    Call xHttp.SetRequestHeader("Pragma", "no-cache")
  End If
  Call xHttp.Send()
  If Not xHttp.Status = 200 Then
    Call WScript.Echo("Unable to access " & Url & " - Error " & xHttp.Status)
    Call WScript.Quit(1)
  End If
  With bStrm
    .type = BINARY
    .open
    .write xHttp.responseBody
    .savetofile File, OVERWRITE
  End With
End Sub

' Unzip a specific file from an archive
Sub Unzip(Archive, File)
  Const NOCONFIRMATION = &H10&
  Const NOERRORUI = &H400&
  Const SIMPLEPROGRESS = &H100&
  unzipFlags = NOCONFIRMATION + NOERRORUI + SIMPLEPROGRESS
  Set objShell = CreateObject("Shell.Application")
  Set objSource = objShell.NameSpace(fso.GetAbsolutePathName(Archive)).Items()
  Set objTarget = objShell.NameSpace(fso.GetAbsolutePathName("."))
  ' Only extract the file we are interested in
  For i = 0 To objSource.Count - 1
    If objSource.Item(i).Name = File Then
      Call objTarget.CopyHere(objSource.Item(i), unzipFlags)
    End If
  Next
End Sub

' Check that QEMU is available
If Not fso.FileExists(QEMU_PATH & QEMU_EXE) Then
  Call WScript.Echo("'" & QEMU_PATH & QEMU_EXE & "' was not found." & vbCrLf &_
    "Please make sure QEMU is installed or edit the path in '.msvc\debug.vbs'.")
  Call WScript.Quit(1)
End If

' Fetch the UEFI firmware and unzip it
If Not fso.FileExists(FW_FILE) Then
  Call WScript.Echo("The UEFI firmware file, needed for QEMU, " &_
    "will be downloaded from: " & FW_URL & vbCrLf & vbCrLf &_
    "Note: Unless you delete the file, this should only happen once.")
  Call DownloadHttp(FW_URL, FW_ZIP)
End If
If Not fso.FileExists(FW_ZIP) And Not fso.FileExists(FW_FILE) Then
  Call WScript.Echo("There was a problem downloading the QEMU UEFI firmware.")
  Call WScript.Quit(1)
End If
If fso.FileExists(FW_ZIP) Then
  Call Unzip(FW_ZIP, FW_BASE & ".fd")
  Call fso.MoveFile(FW_BASE & ".fd", FW_FILE)
  Call fso.DeleteFile(FW_ZIP)
End If
If Not fso.FileExists(FW_FILE) Then
  Call WScript.Echo("There was a problem unzipping the QEMU UEFI firmware.")
  Call WScript.Quit(1)
End If

' Copy the files where required, and start QEMU
Call shell.Run("%COMSPEC% /c mkdir ""image\efi\boot""", 0, True)
Call fso.CopyFile(BIN, "image\efi\boot\" & BOOT_NAME, True)
Call shell.Run("""" & QEMU_PATH & QEMU_EXE & """ " & QEMU_OPTS & " -L . -drive if=pflash,format=raw,unit=0,file=" & FW_FILE & ",readonly=on -drive format=raw,file=fat:rw:image" , 1, True)

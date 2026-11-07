# Xbox storage probe

A separate, tiny x64 UWP app for Xbox Dev Mode. It does not use the engine, SDL,
assets or package identity of Ring Racers. Its identity/publisher is
`RingRacersStorageProbe` / `CN=RingRacersStorageProbe`, so both apps can coexist.
Like the game it targets `Windows.Universal` (including Xbox).

## Install and run

1. Download the **storage-probe** artifact from the **Storage probe** Actions
   workflow on branch `storage-probe`. Extract the signed `.msix` and `.cer`.
2. Install them together through Xbox Device Portal / Dev Home, as with the game.
   Signing uses a new self-signed certificate per CI run; uninstall a previous
   probe build before installing a different run. Uninstalling deletes its local
   logs, counter and ballast. Do not uninstall Ring Racers for this experiment.
3. Connect the USB media drive as `E:` with an existing writable `E:\ringracers`
   folder. Use the repository README's ALL APPLICATION PACKAGES and user ACL
   instructions. The probe does not create or prepare that folder.
4. Launch the probe and keep it in the foreground until the screen changes:
   **amber = running**, **green = finished without errors**, **red = finished
   with errors; inspect the log**. It stays open until closed. Close it fully
   before launching again; resuming the existing process is not a new launch.
5. Read `E:\ringracers\storage-probe.txt` on a PC. Each launch appends a UTC
   timestamp, persistent launch counter, before/after measurements and operation
   results. It also appends a copy to its own LocalState `storage-probe.txt`,
   accessible via Device Portal's app file explorer if USB logging fails.
   USB append errors appear in the local copy; local append errors appear in
   debugger output and turn the screen red.

## Measurements and interpretation

Before and after the ballast operation, the probe measures its LocalState
(`ApplicationData.Current.LocalFolder`), LocalCacheFolder, TemporaryFolder,
`Package.Current.InstalledLocation`, and `E:\ringracers`:

- Full path and `GetDiskFreeSpaceExW` caller free / total / total free, in exact
  bytes and integer MiB (1 MiB = 1,048,576 bytes).
- `CreateFile2` directory handle with `FILE_FLAG_BACKUP_SEMANTICS` through
  `CREATEFILE2_EXTENDED_PARAMETERS`, then
  `GetFileInformationByHandleEx(FileIdInfo).VolumeSerialNumber` (hex) and
  `GetFinalPathNameByHandleW` with `VOLUME_NAME_NT` and `VOLUME_NAME_GUID`.
- Every failed Win32 operation records `GetLastError`; WinRT path lookup failures
  record HRESULT. Even optional GUID/identity failures turn the screen red;
  the other measurements and ballast experiment still continue.

If `LocalState\ballast.bin` is absent, the probe writes **512 MiB** in 1 MiB
chunks using `CreateFile2` / `WriteFile`, flushes with `FlushFileBuffers`, closes
it and remeasures. It uses actual nonzero writes, not a sparse file or merely a
file-length change. On failure it stops writing and reports completed chunks;
a partial file remains for cleanup. If the file already exists (including a
partial file), the next launch deletes it with `DeleteFileW` and remeasures.
The before/after volume measurements bracket the operation with its handle closed.

Use this protocol:

1. Run the probe once (writes ballast). Compare its **before** LocalState volume
   serial, NT device path, total and free space with Ring Racers' prior
   `UWP: data: internal storage: ...` line (about **1842 MiB free of 3071 MiB**
   after copying game data). A matching serial / NT device and free space near
   1842 MiB before and **1842 minus 512 MiB** after suggests shared storage.
   A fresh roughly 3 GiB volume with its own identity suggests a per-app volume.
   The game's current space-only log does not include serial or NT identity;
   compare those only if separately available, and use step 2 as the direct test.
2. While the probe's ballast exists, close the probe and launch Ring Racers.
   If the game's logged internal free space dropped by about **512 MiB** from
   its previous launch, the apps share the space. Preserve the game log before
   launching it again. Account for any other app activity and log growth.
3. Close the game and run the probe again. It deletes ballast and should show
   about 512 MiB reclaimed. Compare **LocalState**, not the installed location
   (the game installs under `S:\Program Files\WindowsApps\...`) or USB volume.

Xbox runtime behavior is not validated by a Windows build: sandbox restrictions
may deny directory handles, FileIdInfo or NT/GUID final paths, and GUID paths may
be unavailable on virtual storage. Those errors are evidence, not omitted data.
Free-space changes across the two apps are useful even if identities are denied.
Caller quotas/virtualization may also make one identity alone inconclusive.
Do not change Dev Mode storage allocation or install/uninstall other apps during
this experiment. The probe intentionally consumes 512 MiB until its next launch
(or uninstall); a write failure may leave a smaller ballast file.

## Build

No submodules or downloads are needed. In an x64 Visual Studio 2022 developer
PowerShell with UWP C++ tools and Windows SDK 10.0.19041.0:

```powershell
./tools/storage-probe/build.ps1
```

The standalone CMake project uses C++17, SDK C++/WinRT headers, WindowsApp,
Direct3D 11 and DXGI. `build.ps1` uses the same signing and MSBuild sideload
packaging procedure as the game's CI, with its own publisher and certificate.
The separate path-filtered workflow does not depend on the game build.

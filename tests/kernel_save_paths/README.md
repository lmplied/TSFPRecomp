# Saves under E:\TDATA and E:\UDATA

Windows regression using temporary folders only; no game files are needed.

XAPI 5849 opens a title's saves by device path
(`\Device\Harddisk0\Partition1\UDATA\<title id>\...`), and those paths went to
the game folder: TimeSplitters: Future Perfect kept every profile beside the
disc's files, outside `RECOMP_SAVE_DIR`. This checks that:

- `Partition1\TDATA` and `Partition1\UDATA` (with or without anything below
  them) translate into the save folder;
- the rest of `Partition1` still translates into the game folder (Wreckless
  loads its assets from there), and the bare device is still the partition
  image;
- saves already in the game folder are copied to the save folder at start-up,
  the game folder's copy is left in place, and a title folder that already
  exists on the save side is not overwritten.

```powershell
cmake -S tests/kernel_save_paths -B build/save-paths -A x64
cmake --build build/save-paths --config Release --target kernel_save_paths_test
ctest --test-dir build/save-paths -C Release --output-on-failure
```

#ifndef DOLRECOMP_DISC_EXTRACT_H
#define DOLRECOMP_DISC_EXTRACT_H

int disc_extract_main(int argc, char** argv);

// Library entry point for hosts that have no command line to synthesise --
// notably iOS, where the extractor runs in-process inside the app.
//
// Extracts a GameCube ISO with the native extractor only (no wit bridge) into
// output_dir, which is created if needed. disc_id receives the 6-character
// disc identifier plus a NUL and so needs 7 bytes; title receives the disc
// header's game name, truncated to 64 characters plus a NUL, so it needs 65.
// Either may be NULL. Returns 1 on success, 0 on failure.
int disc_extract_gamecube(const char* image_path, const char* output_dir,
                          char* disc_id, char* title);

// Read only the disc's identity, without extracting. Touches the first 0x440
// bytes of the image. Returns 1 if this is a GameCube disc, 0 otherwise.
int disc_probe_gamecube(const char* image_path, char* disc_id, char* title);

#endif

// The one symbol a browser has no answer for.
//
// ModernGekko's dolphin_runtime.cpp already implements Dolphin's whole Host_*
// interface, and libusb builds its own null backend for Emscripten, so the
// only gap left is a Linux syscall SFML reaches for because it identifies
// Emscripten as Linux. Nothing in this configuration calls it.
extern "C" {
int sysinfo(void*)
{
  return -1;
}
}

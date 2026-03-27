#ifndef CLIPBOARD_HELPER_H
#define CLIPBOARD_HELPER_H

#ifdef __cplusplus
extern "C" {
#endif

void  Clipboard_Set(const char* text);
// Caller must free() the returned string. Returns NULL on failure.
char* Clipboard_Get(void);

#ifdef __cplusplus
}
#endif

#endif

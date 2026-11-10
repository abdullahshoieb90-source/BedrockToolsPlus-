// Single definition of the stb_image_write implementation for the mod.
//
// Exactly one translation unit may compile the implementation, and every caller
// links against it. This used to live in the Custom Capes module; when that
// module was removed the definition went with it while Wings kept calling
// stbi_write_png(), which left the symbol undefined, made dlopen(RTLD_NOW) fail
// and emptied the entire launcher menu. It lives in its own file now so removing
// a feature can never take the PNG writer with it again.

#define STB_IMAGE_WRITE_IMPLEMENTATION

#include <stb/stb_image_write.h>

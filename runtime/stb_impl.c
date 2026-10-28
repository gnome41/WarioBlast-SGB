/* Implementations of the vendored stb libraries (public domain / MIT),
 * compiled once with warnings off. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "third_party/stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "third_party/stb_image_write.h"

#define STB_VORBIS_NO_STDIO
#include "third_party/stb_vorbis.c"

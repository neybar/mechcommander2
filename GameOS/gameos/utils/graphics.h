#ifndef GRAPHICS_H
#define GRAPHICS_H


#include <GL/glew.h>
#ifndef __APPLE__ // glew.h already declares everything; macOS has no GL/gl.h
#include <GL/gl.h>
#endif

#ifndef PLATFORM_WINDOWS
	// because APIENTRY is undef'ed at the end of glew.h and we use it in shader_builder.h
	#ifndef APIENTRY
		#define APIENTRY
	#endif
#else
	#include<windows.h>
#endif



#endif //GRAPHICS_H

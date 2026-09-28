// Engine is now a class template (see Engine.h, "TVoices"), defined inline in the header so both VoiceEngine
// and ParallelVoiceEngine consumers get their own instantiation without whole-class explicit instantiation.
// This file is kept (rather than deleted) only so existing build commands that name it as a source file, e.g.
// tools/build_proto.sh, keep working unchanged; it contributes no symbols of its own.
#include "Engine.h"

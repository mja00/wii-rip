/*
Copyright (c) 2010 - Wii Banner Player Project

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
claim that you wrote the original software. If you use this software
in a product, an acknowledgment in the product documentation would be
appreciated but is not required.

2. Altered source versions must be plainly marked as such, and must not be
misrepresented as being the original software.

3. This notice may not be removed or altered from any source
distribution.
*/

// NOTE: Altered source version of Sound.cpp from the wii-banner-player project.
// The original used SFML audio (sfml-audio) to play the BNS sound stream.
// This stub replaces that implementation so wii-banner-render can be built
// without SFML. Audio output is intentionally omitted — wii-rip handles
// the disc channel audio separately via its own BNS decoder.

#include <istream>
#include "Sound.h"

namespace WiiBanner
{

// Forward-declared in Sound.h; provide a minimal definition so that the
// destructor can compile even though we never allocate one.
class BannerStream {};

Sound::~Sound()
{
    delete stream;
}

bool Sound::Load(std::istream& /*file*/)
{
    return false;
}

void Sound::Play() {}
void Sound::Pause() {}
void Sound::Stop() {}
void Sound::Restart() {}

}

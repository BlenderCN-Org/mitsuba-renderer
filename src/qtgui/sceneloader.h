/*
    This file is part of Mitsuba, a physically based rendering system.

    Copyright (c) 2007-2011 by Wenzel Jakob and others.

    Mitsuba is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License Version 3
    as published by the Free Software Foundation.

    Mitsuba is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#if !defined(__SCENELOADER_H)
#define __SCENELOADER_H

#include <QtGui>
#include <mitsuba/mitsuba.h>

struct SceneContext;

using namespace mitsuba;

class SceneLoader : public Thread {
public:
	SceneLoader(FileResolver *resolver, const std::string &filename); 
	void run();

	inline void wait(int ms) { m_wait->wait(ms); }

	inline SceneContext *getResult() { return m_result; }
	inline const std::string &getError() const { return m_error; }
protected:
	virtual ~SceneLoader();
private:
	ref<FileResolver> m_resolver;
	ref<WaitFlag> m_wait;
	SceneContext *m_result;
	std::string m_error, m_filename;
};

#endif // __SCENELOADER_H


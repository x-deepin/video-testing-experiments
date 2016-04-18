#include <iostream>
#include <fstream>
#include <sstream>
#include <regex>
#include <string>

#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>

using namespace std;

static string run_and_collect(const string& cmd)
{
    int pfd[2];
    pipe(pfd);
    
    pid_t pid = fork();
    if (pid < 0) {
        return "";
    }

    if (pid > 0) {
        string data;
        close(pfd[1]);

        char buf[129];
        ssize_t sz = 0;
        while ((sz = read(pfd[0], &buf, 128)) > 0) {
            buf[sz] = 0;
            data += buf;
        }

        waitpid(pid, NULL, 0);
        close(pfd[0]);
        return data;

    } else {
        close(pfd[0]);
        dup2(pfd[1], 1);

        execlp(cmd.c_str(), cmd.c_str(), NULL);
    }
    return "";
}

class Checker {
    public:
        virtual int doTest() = 0;
};

class EnvironmentChecker: public Checker {
public:
	struct VideoEnv {
		static const int Unknown     = 0;
		static const int Intel       = 0x0001;
		static const int AMD         = 0x0002;
		static const int Nvidia      = 0x0004;
		static const int VirtualBox  = 0x0100;
		static const int VMWare      = 0x0200;
	};

	int doTest() override {
		if (!isDriverLoadedCorrectly()) {
			return 1;
		}


		_video = VideoEnv::Unknown;

		static regex vbox(".*vga.*virtualbox.*", regex::icase);
		static regex vmware(".*vga.*vmware.*", regex::icase);
		static regex intel(".*(vga|3d).*intel.*", regex::icase);
		static regex amd(".*(vga|3d).*ati.*", regex::icase);
		static regex nvidia(".*(vga|3d).*nvidia.*", regex::icase);

		string data = run_and_collect("lspci");
        std::stringstream ss(data);
        for (string ln; std::getline(ss, ln); ) {
            if (regex_match(ln, vbox)) {
                _video |= VideoEnv::VirtualBox;
            } else if (regex_match(ln, vmware)) {
                _video |= VideoEnv::VMWare;
            } else if (regex_match(ln, intel)) {
                _video |= VideoEnv::Intel;
            } else if (regex_match(ln, amd)) {
                _video |= VideoEnv::AMD;
            } else if (regex_match(ln, nvidia)) {
                _video |= VideoEnv::Nvidia;
            }
        }

		string msg = "video env:";
		if (_video & VideoEnv::VirtualBox) msg += " VirtualBox";
		if (_video & VideoEnv::VMWare) msg += " VMWare";
		if (_video & VideoEnv::Intel) msg += " Intel";
		if (_video & VideoEnv::AMD) msg += " AMD";
		if (_video & VideoEnv::Nvidia) msg += " Nvidia";
        cerr << msg << endl;
        if (_video == VideoEnv::Unknown) {
            return 1;
        }

        //TODO: check lsmod and needed config
        return 0;
	}

    //FIXME: this is too weak, need a better way
	bool isDriverLoadedCorrectly() {
		static regex tests[] = {
            //FIXME: actually I think there are some AIGLX error can be omitted.
            regex(".*(EE)\\s+AIGLX error.*"),
            regex(".*direct rendering.*disabled.*", regex::icase),
            regex(".*GLX: Initialized DRISWRAST GL provider.*", regex::icase),
            //FIXME: DRI v1 is indirect rendering, should not be used.
            regex(".*direct rendering: DRI enabled.*", regex::icase),
        };

        std::ifstream f("/var/log/Xorg.0.log");
		for (string ln; std::getline(f, ln); ) {
            for (auto& r: tests) {
                if (regex_match(ln, r)) {
                    return false;
                }
            }
		}

		return true;
	}

private:
	int _video {VideoEnv::Unknown};
};

class ExtensionChecker: public Checker {
public:
    int doTest() override {

        int ret = 0;

        Display* display = XOpenDisplay(NULL);
        if (!display) {
            return 1;
        }

        if ((ret = testComposite(display))) {
            goto _error_out;
        }

        if ((ret = testDamage(display))) {
            goto _error_out;
        }

_error_out:
        XCloseDisplay(display);
        return ret;
    }

private:
    int testDamage(Display* display) {
        int damage_event_base;
        int damage_error_base;
        if (!XDamageQueryExtension(display, &damage_event_base,
                    &damage_error_base)) {
            return 1;
        }

        return 0;
    }

    //TODO: do explicit Composite testing: i.e test NameWindowPixmap
    int testComposite(Display* display) {
        int composite_event_base;
        int composite_error_base;
        int composite_major_version;
        int composite_minor_version;

        int ret = 0;

        if (!XCompositeQueryExtension(display, &composite_event_base,
                    &composite_error_base)) {
            return 1;
        } else {
            composite_major_version = 0;
            composite_minor_version = 0;
            if (!XCompositeQueryVersion(display, &composite_major_version,
                        &composite_minor_version)) {
            return 1;
            }
        }
        return 0;
    }
};

int main(int argc, char *argv[])
{
    Checker* checkers[] = {
        new EnvironmentChecker(),
        new ExtensionChecker(),
    };

    for (int i = 0; i < 2; i++) {
        if (checkers[i]->doTest()) 
            return 1;
    }
    return 0;
}


#include <sys/inotify.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <iostream>
#include <fstream>
#include <unordered_map>
#include <vector>
#include <string>

void usage(char** argv) {
	std::cerr << "Usage: " << argv[0]
		<< " [--nostdout] [--nostderr] <string> <timeout> <command> <file1> [file2 ...]"
		<< std::endl;
}

struct LogState {
	off_t offset = 0;
	std::string path;
	std::string buffer;
};

int main(int argc, char** argv) {
	if(argc < 5) {
		usage(argv);
		return 1;
	}

	int argi = 1;
	bool nostdout = false;
	bool nostderr = false;
	for(; argi < argc; argi++) {
		std::string arg = argv[argi];
		if(arg == "--nostdout") {
			nostdout = true;
		} else if(arg == "--nostderr") {
			nostderr = true;
		} else {
			break;
		}
	}

	if(argc - argi < 4) {
		usage(argv);
		return 1;
	}

	std::string match = argv[argi++];
	int timeout;
	try {
		timeout = std::stoi(argv[argi++]);
	} catch(...) {
		if(nostderr == false) {
			std::cerr << "Error: invalid arg timeout" << std::endl;
		}
		return 1;
	}
	std::string command = argv[argi++];
	std::vector<std::string> files;
	for(; argi < argc; argi++) {
		files.push_back(argv[argi]);
	};

	int inotify_fd = inotify_init1(IN_NONBLOCK);
	if(inotify_fd < 0) {
		if(nostderr == false) {
			std::cerr << "Error: inotify_init1 failed" << std::endl;
		}
		return 1;
	}

	std::unordered_map<std::string, LogState> states;
	for(auto &f : files) {
		states[f] = LogState{0, f, ""};
		std::string dir = f.substr(0, f.find_last_of('/'));
		int wd = inotify_add_watch(inotify_fd, dir.c_str(), IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
		if(wd < 0) {
			if(nostderr == false) {
				std::cerr << "Error: inotify_add_watch failed" << std::endl;
			}
			return 1;
		}
	}
	
	while(true) {
		for(auto &pair : states) {
			LogState &s = pair.second;
			struct stat st;
			if(stat(s.path.c_str(), &st) != 0) {
				if(nostdout == false) {
					std::cout << "File " << s.path << " not found, skipping" << std::endl;
				}
				continue;
			}

			if(st.st_size < s.offset) {
				if(nostderr == false) {
					std::cerr << "Error: File " << s.path << " shrunk, giving up" << std::endl;
				}
				return 1;
			}

			if(st.st_size > s.offset) {
				std::ifstream infile(s.path);
				infile.seekg(s.offset);
				std::string line;
				while(std::getline(infile, line)) {
					if(!s.buffer.empty()) {
						line = s.buffer + line;
						s.buffer.clear();
					}

					if(!line.empty() && line.back() != '\n' && infile.eof()) {
						s.buffer = line;
						break;
					}

					if(line.find(match) != std::string::npos) {
						if(nostdout == false) {
							std::cout << line << "\n";
						}

						std::string line_safe;
						for(char c :line) {
							if(c == '"' || c == '\\') {
								line_safe.push_back('\\');
							}
							line_safe.push_back(c);
						}

						std::string cmd = command + " \"" + line_safe + "\"";
						int ret = system(cmd.c_str());
						if(ret == -1) {
							if(nostderr == false) {
								std::cerr << "Error: Command failed" << std::endl;
							}
						}
					}
				}

				s.offset = st.st_size;
			}
		}

		char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
		ssize_t len = read(inotify_fd, buf, sizeof(buf));
		if(len > 0) {
			for(char* ptr = buf; ptr < buf + len;) {
				struct inotify_event* event = (struct inotify_event*) ptr;
				for(auto &pair : states) {
					LogState &s = pair.second;
					std::string filename = s.path.substr(s.path.find_last_of('/') + 1);
					if(event->len && filename == event->name && (event->mask & (IN_DELETE | IN_MOVED_FROM | IN_CREATE | IN_MOVED_TO))) {
						if(nostdout == false) {
							std::cout << "File " << s.path << " disappeared, resetting offset\n";
						}
						s.offset = 0;
					}
				}
				ptr += sizeof(struct inotify_event) + event->len;
			}
		}

		usleep(timeout * 1000000);
	}

	close(inotify_fd);
	return 0;
}

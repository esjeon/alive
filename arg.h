/*
 * copied from sbase ( http://git.suckless.org/sbase/ )
 */
/*
 * Copy me if you can.
 * by 20h
 */

#ifndef __ARG_H__
#define __ARG_H__

extern char *argv0;

/* use main(int argc, char *argv[]) */
#define ARGBEGIN	for (argv0 = *argv, argv++, argc--;\
					argv[0] && argv[0][1]\
					&& argv[0][0] == '-';\
					argc--, argv++) {\
				char _argc;\
				char **_argv;\
				int _brk;\
				if (argv[0][1] == '-' && argv[0][2] == '\0') {\
					argv++;\
					argc--;\
					break;\
				}\
				for (_brk = 0, argv[0]++, _argv = argv;\
						argv[0][0] && !_brk;\
						argv[0]++) {\
					if (_argv != argv)\
						break;\
					_argc = argv[0][0];\
					switch (_argc)

/* Handles obsolete -NUM syntax */
#define ARGNUM				case '0':\
					case '1':\
					case '2':\
					case '3':\
					case '4':\
					case '5':\
					case '6':\
					case '7':\
					case '8':\
					case '9'

#define ARGEND			}\
			}

#define ARGC()		_argc

#define ARGNUMF(base)	(_brk = 1, estrtol(argv[0], (base)))

#define EARGF(x)	((argv[0][1] == '\0' && argv[1] == NULL)?\
				((x), abort(), (char *)0) :\
				(_brk = 1, (argv[0][1] != '\0')?\
					(&argv[0][1]) :\
					(argc--, argv++, argv[0])))

#define ARGF()		((argv[0][1] == '\0' && argv[1] == NULL)?\
				(char *)0 :\
				(_brk = 1, (argv[0][1] != '\0')?\
					(&argv[0][1]) :\
					(argc--, argv++, argv[0])))

#endif

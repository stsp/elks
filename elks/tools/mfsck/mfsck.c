/*
 * mfsck.c - a file system consistency checker for Linux.
 * 2019-04-26 - Slightly modified from fsck.minix.c for portability greg@censoft.com
 *
 * (C) 1991, 1992 Linus Torvalds. This file may be redistributed
 * as per the GNU copyleft.
 */

/*
 * 09.11.91  -  made the first rudimetary functions
 *
 * 10.11.91  -  updated, does checking, no repairs yet.
 *		Sent out to the mailing-list for testing.
 *
 * 14.11.91  -	Testing seems to have gone well. Added some
 *		correction-code, and changed some functions.
 *
 * 15.11.91  -  More correction code. Hopefully it notices most
 *		cases now, and tries to do something about them.
 *
 * 16.11.91  -  More corrections (thanks to Mika Jalava). Most
 *		things seem to work now. Yeah, sure.
 *
 *
 * 19.04.92  -	Had to start over again from this old version, as a
 *		kernel bug ate my enhanced fsck in february.
 *
 * 28.02.93  -	added support for different directory entry sizes..
 *
 * Sat Mar  6 18:59:42 1993, faith@cs.unc.edu: Output namelen with
 *                           super-block information
 *
 * Sat Oct  9 11:17:11 1993, faith@cs.unc.edu: make exit status conform
 *                           to that required by fsutil
 *
 * Mon Jan  3 11:06:52 1994 - Dr. Wettstein (greg%wind.uucp@plains.nodak.edu)
 *			      Added support for file system valid flag.  Also
 *			      added program_version variable and output of
 *			      program name and version number when program
 *			      is executed.
 *
 * 30.10.94 - added support for v2 filesystem
 *            (Andreas Schwab, schwab@issan.informatik.uni-dortmund.de)
 *
 * 10.12.94  -  added test to prevent checking of mounted fs adapted
 *              from Theodore Ts'o's (tytso@athena.mit.edu) e2fsck
 *              program.  (Daniel Quinlan, quinlan@yggdrasil.com)
 *
 * 01.07.96  - Fixed the v2 fs stuff to use the right #defines and such
 *	       for modern libcs (janl@math.uio.no, Nicolai Langfeldt)
 *
 * 02.07.96  - Added C bit fiddling routines from rmk@ecs.soton.ac.uk 
 *             (Russell King).  He made them for ARM.  It would seem
 *	       that the ARM is powerful enough to do this in C whereas
 *             i386 and m64k must use assembly to get it fast >:-)
 *	       This should make minix fsck systemindependent.
 *	       (janl@math.uio.no, Nicolai Langfeldt)
 *
 * 04.11.96  - Added minor fixes from Andreas Schwab to avoid compiler
 *             warnings.  Added mc68k bitops from 
 *	       Joerg Dorchain <dorchain@mpi-sb.mpg.de>.
 *
 * 06.11.96  - Added v2 code submitted by Joerg Dorchain, but written by
 *             Andreas Schwab.
 *
 * 1999-02-22 Arkadiusz Mi¶kiewicz <misiek@pld.ORG.PL>
 * - added Native Language Support
 *
 * 2008-04-06 James Youngman <jay@gnu.org>
 * - Issue better error message if we fail to open the device.
 * - Restore terminal state if we get a fatal signal.
 *
 *
 * I've had no time to add comments - hopefully the function names
 * are comments enough. As with all file system checkers, this assumes
 * the file system is quiescent - don't use it on a mounted device
 * unless you can be sure nobody is writing to it (and remember that the
 * kernel can write to it when it searches for files).
 *
 * Usuage: fsck [-larvsm] device
 *	-l for a listing of all the filenames
 *	-a for automatic repairs (not implemented)
 *	-r for repairs (interactive) (not implemented)
 *	-v for verbose (tells how many files)
 *	-s for super-block info
 *	-m for minix-like "mode not cleared" warnings
 *	-f force filesystem check even if filesystem marked as valid
 *
 * The device may be a block device or a image of one, but this isn't
 * enforced (but it's not much fun on a character device :-). 
 */

#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdlib.h>
#include <termios.h>
#include <sys/stat.h>
#include <signal.h>
#if LINUX
#include <mntent.h>
#endif

#include "mfsck.h"
#include "bitops.h"

#define PACKAGE_STRING	"2.6"
#define _(str)	str

#define ROOT_INO 1

#define UPPER(size,n) ((size+((n)-1))/(n))
#define INODE_SIZE (sizeof(struct minix_inode))
#define INODE_SIZE2 (sizeof(struct minix2_inode))
#define INODE_BLOCKS UPPER(INODES, (version2 ? MINIX2_INODES_PER_BLOCK \
				    : MINIX_INODES_PER_BLOCK))
#define INODE_BUFFER_SIZE (INODE_BLOCKS * BLOCK_SIZE)

#define BITS_PER_BLOCK (BLOCK_SIZE<<3)

static char * program_name = "fsck.minix";
static char * device_name = NULL;
static int IN;
static int repair=0, automatic=0, verbose=1, list=0, show=0, warn_mode=0, 
	force=1;
static int directory=0, regular=0, blockdev=0, chardev=0, links=0,
		symlinks=0, total=0;

static int changed = 0; /* flags if the filesystem has been changed */
static int errors_uncorrected = 0; /* flag if some error was not corrected */
static int dirsize = 16;
static int namelen = 14;
static int version2 = 0;
static struct termios termios;
static volatile sig_atomic_t termios_set = 0;

/* File-name data */
#define MAX_DEPTH 50
static int name_depth = 0;
static char name_list[MAX_DEPTH][NAME_MAX+1];
/* Copy of the previous, just for error reporting - see get_current_name */
/* This is a waste of 12kB or so. */
static char current_name[MAX_DEPTH*(NAME_MAX+1)+1];

static char * inode_buffer = NULL;
#define Inode (((struct minix_inode *) inode_buffer)-1)
#define Inode2 (((struct minix2_inode *) inode_buffer)-1)

static char *super_block_buffer;
#define Super (*(struct minix_super_block *)super_block_buffer)
#define INODES ((unsigned long)Super.s_ninodes)
#define ZONES ((unsigned long)(version2 ? Super.s_zones : Super.s_nzones))
#define IMAPS ((unsigned long)Super.s_imap_blocks)
#define ZMAPS ((unsigned long)Super.s_zmap_blocks)
#define FIRSTZONE ((unsigned long)Super.s_firstdatazone)
#define ZONESIZE ((unsigned long)Super.s_log_zone_size)
#define MAXSIZE ((unsigned long)Super.s_max_size)
#define MAGIC (Super.s_magic)
#define NORM_FIRSTZONE (2+IMAPS+ZMAPS+INODE_BLOCKS)

static char *inode_map;
static char *zone_map;

static unsigned char * inode_count = NULL;
static unsigned char * zone_count = NULL;

static void recursive_check(unsigned int ino);
static void recursive_check2(unsigned int ino);

#define inode_in_use(x) (bit(inode_map,(x)) != 0)
#define zone_in_use(x) (bit(zone_map,(x)-FIRSTZONE+1) != 0)

#define mark_inode(x) (setbit(inode_map,(x)),changed=1)
#define unmark_inode(x) (clrbit(inode_map,(x)),changed=1)

#define mark_zone(x) (setbit(zone_map,(x)-FIRSTZONE+1),changed=1)
#define unmark_zone(x) (clrbit(zone_map,(x)-FIRSTZONE+1),changed=1)

static void
reset(void) {
	if (termios_set)
		tcsetattr(0, TCSANOW, &termios);
}


static void
fatalsig(int sig) {
	/* We received a fatal signal.  Reset the terminal.
	 * Also reset the signal handler and re-send the signal,
	 * so that the parent process knows which signal actually
	 * caused our death.
	 */
	signal(sig, SIG_DFL);
	reset();
	raise(sig);
}

static void
leave(int status) {
        reset();
	exit(status);
}

static void
usage(void) {
	fprintf(stderr,
		_("Usage: %s [-larvsmf] /dev/name\n"),
		program_name);
	leave(16);
}

//static void die(const char *fmt, ...)
	//__attribute__ ((__format__ (__printf__, 1, 2)));

static void
die(const char *fmt, ...) {
	va_list ap;

	fprintf(stderr, "%s: ", program_name);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end (ap);
	fputc('\n', stderr);
	leave(8);
}

/*
 * This simply goes through the file-name data and prints out the
 * current file.
 */
static void
get_current_name(void) {
	int i = 0, ct;
	char *p, *q;

	q = current_name;
	while (i < name_depth) {
		p = name_list[i++];
		ct = namelen;
		*q++ = '/';
		while (ct-- && *p)
			*q++ = *p++;
	}
	if (i == 0)
		*q++ = '/';
	*q = 0;
}

static int
ask(const char * string, int def) {
	int c;

	if (!repair) {
		printf("\n");
		errors_uncorrected = 1;
		return 0;
	}
	if (automatic) {
		printf("\n");
		if (!def)
		      errors_uncorrected = 1;
		return def;
	}
	printf(def?"%s (y/n)? ":"%s (n/y)? ",string);
	for (;;) {
		fflush(stdout);
		if ((c=getchar())==EOF) {
		        if (!def)
			      errors_uncorrected = 1;
			return def;
		}
		c=toupper(c);
		if (c == 'Y') {
			def = 1;
			break;
		} else if (c == 'N') {
			def = 0;
			break;
		} else if (c == ' ' || c == '\n')
			break;
	}
	if (def)
		printf("y\n");
	else {
		printf("n\n");
		errors_uncorrected = 1;
	     }
	return def;
}

#if LINUX
/*
 * Make certain that we aren't checking a filesystem that is on a
 * mounted partition.  Code adapted from e2fsck, Copyright (C) 1993,
 * 1994 Theodore Ts'o.  Also licensed under GPL.
 */
static void
check_mount(void) {
	FILE * f;
	struct mntent * mnt;
	int cont;
	int fd;

	if ((f = setmntent (_PATH_MOUNTED, "r")) == NULL)
		return;
	while ((mnt = getmntent (f)) != NULL)
		if (strcmp (device_name, mnt->mnt_fsname) == 0)
			break;
	endmntent (f);
	if (!mnt)
		return;

	/*
	 * If the root is mounted read-only, then /etc/mtab is
	 * probably not correct; so we won't issue a warning based on
	 * it.
	 */
	fd = open(_PATH_MOUNTED, O_RDWR);
	if (fd < 0 && errno == EROFS)
		return;
	else
		close(fd);
	
	printf (_("%s is mounted.	 "), device_name);
	if (isatty(0) && isatty(1))
		cont = ask(_("Do you really want to continue"), 0);
	else
		cont = 0;
	if (!cont) {
		printf (_("check aborted.\n"));
		exit (0);
	}
	return;
}
#endif

/*
 * check_zone_nr checks to see that *nr is a valid zone nr. If it
 * isn't, it will possibly be repaired. Check_zone_nr sets *corrected
 * if an error was corrected, and returns the zone (0 for no zone
 * or a bad zone-number).
 */
static int
check_zone_nr(unsigned short * nr, int * corrected) {
	if (!*nr)
		return 0;

	if (*nr < FIRSTZONE) {
		get_current_name();
		printf(_("Zone nr < FIRSTZONE in file `%s'."),
		       current_name);
	} else if (*nr >= ZONES) {
		get_current_name();
		printf(_("Zone nr >= ZONES in file `%s'."),
		       current_name);
	} else
		return *nr;

	if (ask(_("Remove block"),1)) {
		*nr = 0;
		*corrected = 1;
	}
	return 0;
}

static int
check_zone_nr2 (unsigned int *nr, int *corrected) {
	if (!*nr)
		return 0;

	if (*nr < FIRSTZONE) {
		get_current_name();
		printf (_("Zone nr < FIRSTZONE in file `%s'."),
			current_name);
	} else if (*nr >= ZONES) {
		get_current_name();
		printf (_("Zone nr >= ZONES in file `%s'."),
			current_name);
	} else
		return *nr;

	if (ask (_("Remove block"), 1)) {
		*nr = 0;
		*corrected = 1;
	}
	return 0;
}

/*
 * read-block reads block nr into the buffer at addr.
 */
static void
read_block(unsigned int nr, char * addr) {
	if (!nr) {
		memset(addr,0,BLOCK_SIZE);
		return;
	}
	if (BLOCK_SIZE*nr != lseek(IN, BLOCK_SIZE*nr, SEEK_SET)) {
		get_current_name();
		printf(_("Read error: unable to seek to block in file '%s'\n"),
		       current_name);
		memset(addr,0,BLOCK_SIZE);
		errors_uncorrected = 1;
	} else if (BLOCK_SIZE != read(IN, addr, BLOCK_SIZE)) {
		get_current_name();
		printf(_("Read error: bad block in file '%s'\n"),
		       current_name);
		memset(addr,0,BLOCK_SIZE);
		errors_uncorrected = 1;
	}
}

/*
 * write_block writes block nr to disk.
 */
static void
write_block(unsigned int nr, char * addr) {
	if (!nr)
		return;
	if (nr < FIRSTZONE || nr >= ZONES) {
		printf(_("Internal error: trying to write bad block\n"
		"Write request ignored\n"));
		errors_uncorrected = 1;
		return;
	}
	if (BLOCK_SIZE*nr != lseek(IN, BLOCK_SIZE*nr, SEEK_SET))
		die(_("seek failed in write_block"));
	if (BLOCK_SIZE != write(IN, addr, BLOCK_SIZE)) {
		get_current_name();
		printf(_("Write error: bad block in file '%s'\n"),
		       current_name);
		errors_uncorrected = 1;
	}
}

/*
 * map-block calculates the absolute block nr of a block in a file.
 * It sets 'changed' if the inode has needed changing, and re-writes
 * any indirect blocks with errors.
 */
static int
map_block(struct minix_inode * inode, unsigned int blknr) {
	unsigned short ind[BLOCK_SIZE>>1];
	unsigned short dind[BLOCK_SIZE>>1];
	int blk_chg, block, result;

	if (blknr<7)
		return check_zone_nr(inode->i_zone + blknr, &changed);
	blknr -= 7;
	if (blknr<512) {
		block = check_zone_nr(inode->i_zone + 7, &changed);
		read_block(block, (char *) ind);
		blk_chg = 0;
		result = check_zone_nr(blknr + ind, &blk_chg);
		if (blk_chg)
			write_block(block, (char *) ind);
		return result;
	}
	blknr -= 512;
	block = check_zone_nr(inode->i_zone + 8, &changed);
	read_block(block, (char *) dind);
	blk_chg = 0;
	result = check_zone_nr(dind + (blknr/512), &blk_chg);
	if (blk_chg)
		write_block(block, (char *) dind);
	block = result;
	read_block(block, (char *) ind);
	blk_chg = 0;
	result = check_zone_nr(ind + (blknr%512), &blk_chg);
	if (blk_chg)
		write_block(block, (char *) ind);
	return result;
}

static int
map_block2 (struct minix2_inode *inode, unsigned int blknr) {
  	unsigned int ind[BLOCK_SIZE >> 2];
	unsigned int dind[BLOCK_SIZE >> 2];
	unsigned int tind[BLOCK_SIZE >> 2];
	int blk_chg, block, result;

	if (blknr < 7)
		return check_zone_nr2 (inode->i_zone + blknr, &changed);
	blknr -= 7;
	if (blknr < 256) {
		block = check_zone_nr2 (inode->i_zone + 7, &changed);
		read_block (block, (char *) ind);
		blk_chg = 0;
		result = check_zone_nr2 (blknr + ind, &blk_chg);
		if (blk_chg)
			write_block (block, (char *) ind);
		return result;
	}
	blknr -= 256;
	if (blknr >= 256 * 256) {
		block = check_zone_nr2 (inode->i_zone + 8, &changed);
		read_block (block, (char *) dind);
		blk_chg = 0;
		result = check_zone_nr2 (dind + blknr / 256, &blk_chg);
		if (blk_chg)
			write_block (block, (char *) dind);
		block = result;
		read_block (block, (char *) ind);
		blk_chg = 0;
		result = check_zone_nr2 (ind + blknr % 256, &blk_chg);
		if (blk_chg)
			write_block (block, (char *) ind);
		return result;
	}
	blknr -= 256 * 256;
	block = check_zone_nr2 (inode->i_zone + 9, &changed);
	read_block (block, (char *) tind);
	blk_chg = 0;
	result = check_zone_nr2 (tind + blknr / (256 * 256), &blk_chg);
	if (blk_chg)
		write_block (block, (char *) tind);
	block = result;
	read_block (block, (char *) dind);
	blk_chg = 0;
	result = check_zone_nr2 (dind + (blknr / 256) % 256, &blk_chg);
	if (blk_chg)
		write_block (block, (char *) dind);
	block = result;
	read_block (block, (char *) ind);
	blk_chg = 0;
	result = check_zone_nr2 (ind + blknr % 256, &blk_chg);
	if (blk_chg)
		write_block (block, (char *) ind);
	return result;
}

static void
write_super_block(void) {
	/*
	 * Set the state of the filesystem based on whether or not there
	 * are uncorrected errors.  The filesystem valid flag is
	 * unconditionally set if we get this far.
	 */
	Super.s_state |= MINIX_VALID_FS;
	if ( errors_uncorrected )
		Super.s_state |= MINIX_ERROR_FS;
	else
		Super.s_state &= ~MINIX_ERROR_FS;
	
	if (BLOCK_SIZE != lseek(IN, BLOCK_SIZE, SEEK_SET))
		die(_("seek failed in write_super_block"));
	if (BLOCK_SIZE != write(IN, super_block_buffer, BLOCK_SIZE))
		die(_("unable to write super-block"));

	return;
}

static void
write_tables(void) {
	write_super_block();

	if (IMAPS*BLOCK_SIZE != write(IN,inode_map,IMAPS*BLOCK_SIZE))
		die(_("Unable to write inode map"));
	if (ZMAPS*BLOCK_SIZE != write(IN,zone_map,ZMAPS*BLOCK_SIZE))
		die(_("Unable to write zone map"));
	if (INODE_BUFFER_SIZE != write(IN,inode_buffer,INODE_BUFFER_SIZE))
		die(_("Unable to write inodes"));
}

static void
get_dirsize (void) {
	int block;
	char blk[BLOCK_SIZE];
	int size;

	if (version2)
		block = Inode2[ROOT_INO].i_zone[0];
	else
		block = Inode[ROOT_INO].i_zone[0];
	read_block (block, blk);
	for (size = 16; size < BLOCK_SIZE; size <<= 1) {
		if (strcmp (blk + size + 2, "..") == 0) {
			dirsize = size;
			namelen = size - 2;
			return;
		}
	}
	/* use defaults */
}

static void
read_superblock(void) {
	if (BLOCK_SIZE != lseek(IN, BLOCK_SIZE, SEEK_SET))
		die(_("seek failed"));

	super_block_buffer = calloc(1, BLOCK_SIZE);
	if (!super_block_buffer)
		die(_("unable to alloc buffer for superblock"));

	if (BLOCK_SIZE != read(IN, super_block_buffer, BLOCK_SIZE))
		die(_("unable to read super block"));
	if (MAGIC == MINIX_SUPER_MAGIC) {
		namelen = 14;
		dirsize = 16;
		version2 = 0;
	} else if (MAGIC == MINIX_SUPER_MAGIC2) {
		namelen = 30;
		dirsize = 32;
		version2 = 0;
	} else if (MAGIC == MINIX2_SUPER_MAGIC) {
		namelen = 14;
		dirsize = 16;
		version2 = 1;
	} else if (MAGIC == MINIX2_SUPER_MAGIC2) {
		namelen = 30;
		dirsize = 32;
		version2 = 1;
	} else
		die(_("bad magic number in super-block"));
	if (ZONESIZE != 0 || BLOCK_SIZE != 1024)
		die(_("Only 1k blocks/zones supported"));
	if (IMAPS * BLOCK_SIZE * 8 < INODES + 1)
		die(_("bad s_imap_blocks field in super-block"));
	if (ZMAPS * BLOCK_SIZE * 8 < ZONES - FIRSTZONE + 1)
		die(_("bad s_zmap_blocks field in super-block"));
}

static void
read_tables(void) {
	inode_map = malloc(IMAPS * BLOCK_SIZE);
	if (!inode_map)
		die(_("Unable to allocate buffer for inode map"));
	zone_map = malloc(ZMAPS * BLOCK_SIZE);
	if (!inode_map)
		die(_("Unable to allocate buffer for zone map"));
	memset(inode_map,0,IMAPS * BLOCK_SIZE);
	memset(zone_map,0,ZMAPS * BLOCK_SIZE);
	inode_buffer = malloc(INODE_BUFFER_SIZE);
	if (!inode_buffer)
		die(_("Unable to allocate buffer for inodes"));
	inode_count = malloc(INODES + 1);
	if (!inode_count)
		die(_("Unable to allocate buffer for inode count"));
	zone_count = malloc(ZONES);
	if (!zone_count)
		die(_("Unable to allocate buffer for zone count"));
	if (IMAPS*BLOCK_SIZE != read(IN,inode_map,IMAPS*BLOCK_SIZE))
		die(_("Unable to read inode map"));
	if (ZMAPS*BLOCK_SIZE != read(IN,zone_map,ZMAPS*BLOCK_SIZE))
		die(_("Unable to read zone map"));
	if (INODE_BUFFER_SIZE != read(IN,inode_buffer,INODE_BUFFER_SIZE))
		die(_("Unable to read inodes"));
	if (NORM_FIRSTZONE != FIRSTZONE) {
		printf(_("Warning: Firstzone != Norm_firstzone\n"));
		errors_uncorrected = 1;
	}
	get_dirsize ();
	if (show) {
		printf(_("%ld inodes\n"),INODES);
		printf(_("%ld blocks\n"),ZONES);
		printf(_("Firstdatazone=%ld (%ld)\n"),FIRSTZONE,NORM_FIRSTZONE);
		printf(_("Zonesize=%d\n"),BLOCK_SIZE<<ZONESIZE);
		printf(_("Maxsize=%ld\n"),MAXSIZE);
		printf(_("Filesystem state=%d\n"), Super.s_state);
		printf(_("namelen=%d\n\n"),namelen);
	}
}

static struct minix_inode *
get_inode(unsigned int nr) {
	struct minix_inode * inode;

	if (!nr || nr > INODES)
		return NULL;
	total++;
	inode = Inode + nr;
	if (!inode_count[nr]) {
		if (!inode_in_use(nr)) {
			get_current_name();
			printf(_("Inode %d marked unused, "
				 "but used for file '%s'\n"),
			       nr, current_name);
			if (repair) {
				if (ask(_("Mark in use"),1))
					mark_inode(nr);
			} else {
			        errors_uncorrected = 1;
			}
		}
		if (S_ISDIR(inode->i_mode))
			directory++;
		else if (S_ISREG(inode->i_mode))
			regular++;
		else if (S_ISCHR(inode->i_mode))
			chardev++;
		else if (S_ISBLK(inode->i_mode))
			blockdev++;
		else if (S_ISLNK(inode->i_mode))
			symlinks++;
		else if (S_ISSOCK(inode->i_mode))
			;
		else if (S_ISFIFO(inode->i_mode))
			;
		else {
                        get_current_name();
                        printf(_("The file `%s' has mode %05o\n"),
			       current_name, inode->i_mode);
                }

	} else
		links++;
	if (!++inode_count[nr]) {
		printf(_("Warning: inode count too big.\n"));
		inode_count[nr]--;
		errors_uncorrected = 1;
	}
	return inode;
}

static struct minix2_inode *
get_inode2 (unsigned int nr) {
	struct minix2_inode *inode;

	if (!nr || nr > INODES)
		return NULL;
	total++;
	inode = Inode2 + nr;
	if (!inode_count[nr]) {
		if (!inode_in_use (nr)) {
			get_current_name();
			printf (_("Inode %d marked unused, "
				  "but used for file '%s'\n"),
				nr, current_name);
			if (repair) {
				if (ask (_("Mark in use"), 1))
					mark_inode (nr);
				else
					errors_uncorrected = 1;
			}
		}
		if (S_ISDIR (inode->i_mode))
			directory++;
		else if (S_ISREG (inode->i_mode))
			regular++;
		else if (S_ISCHR (inode->i_mode))
			chardev++;
		else if (S_ISBLK (inode->i_mode))
			blockdev++;
		else if (S_ISLNK (inode->i_mode))
			symlinks++;
		else if (S_ISSOCK (inode->i_mode));
		else if (S_ISFIFO (inode->i_mode));
		else {
			get_current_name ();
			printf (_("The file `%s' has mode %05o\n"),
				current_name, inode->i_mode);
		}
	} else
		links++;
	if (!++inode_count[nr]) {
		printf (_("Warning: inode count too big.\n"));
		inode_count[nr]--;
		errors_uncorrected = 1;
	}
	return inode;
}

static void
check_root(void) {
	struct minix_inode * inode = Inode + ROOT_INO;

	if (!inode || !S_ISDIR(inode->i_mode))
		die(_("root inode isn't a directory"));
}

static void
check_root2 (void) {
	struct minix2_inode *inode = Inode2 + ROOT_INO;

	if (!inode || !S_ISDIR (inode->i_mode))
		die(_("root inode isn't a directory"));
}

static int
add_zone(unsigned short * znr, int * corrected) {
	int result;
	int block;

	result = 0;
	block = check_zone_nr(znr, corrected);
	if (!block)
		return 0;
	if (zone_count[block]) {
		get_current_name();
		printf(_("Block has been used before. Now in file `%s'."),
		       current_name);
		if (ask(_("Clear"),1)) {
			*znr = 0;
			block = 0;
			*corrected = 1;
		}
	}
	if (!block)
		return 0;
	if (!zone_in_use(block)) {
		get_current_name();
		printf(_("Block %d in file `%s' is marked not in use."),
		       block, current_name);
		if (ask(_("Correct"),1))
			mark_zone(block);
	}
	if (!++zone_count[block])
		zone_count[block]--;
	return block;
}

static int
add_zone2 (unsigned int *znr, int *corrected) {
	int result;
	int block;

	result = 0;
	block = check_zone_nr2 (znr, corrected);
	if (!block)
		return 0;
	if (zone_count[block]) {
		get_current_name();
		printf (_("Block has been used before. Now in file `%s'."),
			current_name);
		if (ask (_("Clear"), 1)) {
			*znr = 0;
			block = 0;
			*corrected = 1;
		}
	}
	if (!block)
		return 0;
	if (!zone_in_use (block)) {
		get_current_name();
		printf (_("Block %d in file `%s' is marked not in use."),
			block, current_name);
		if (ask (_("Correct"), 1))
			mark_zone (block);
	}
	if (!++zone_count[block])
		zone_count[block]--;
	return block;
}

static void
add_zone_ind(unsigned short * znr, int * corrected) {
	static char blk[BLOCK_SIZE];
	int i, chg_blk=0;
	int block;

	block = add_zone(znr, corrected);
	if (!block)
		return;
	read_block(block, blk);
	for (i=0 ; i < (BLOCK_SIZE>>1) ; i++)
		add_zone(i + (unsigned short *) blk, &chg_blk);
	if (chg_blk)
		write_block(block, blk);
}

static void
add_zone_ind2 (unsigned int *znr, int *corrected) {
	static char blk[BLOCK_SIZE];
	int i, chg_blk = 0;
	int block;

	block = add_zone2 (znr, corrected);
	if (!block)
		return;
	read_block (block, blk);
	for (i = 0; i < BLOCK_SIZE >> 2; i++)
		add_zone2 (i + (unsigned int *) blk, &chg_blk);
	if (chg_blk)
		write_block (block, blk);
}

static void
add_zone_dind(unsigned short * znr, int * corrected) {
	static char blk[BLOCK_SIZE];
	int i, blk_chg=0;
	int block;

	block = add_zone(znr, corrected);
	if (!block)
		return;
	read_block(block, blk);
	for (i=0 ; i < (BLOCK_SIZE>>1) ; i++)
		add_zone_ind(i + (unsigned short *) blk, &blk_chg);
	if (blk_chg)
		write_block(block, blk);
}

static void
add_zone_dind2 (unsigned int *znr, int *corrected) {
	static char blk[BLOCK_SIZE];
	int i, blk_chg = 0;
	int block;

	block = add_zone2 (znr, corrected);
	if (!block)
		return;
	read_block (block, blk);
	for (i = 0; i < BLOCK_SIZE >> 2; i++)
		add_zone_ind2 (i + (unsigned int *) blk, &blk_chg);
	if (blk_chg)
		write_block (block, blk);
}

static void
add_zone_tind2 (unsigned int *znr, int *corrected) {
	static char blk[BLOCK_SIZE];
	int i, blk_chg = 0;
	int block;

	block = add_zone2 (znr, corrected);
	if (!block)
		return;
	read_block (block, blk);
	for (i = 0; i < BLOCK_SIZE >> 2; i++)
		add_zone_dind2 (i + (unsigned int *) blk, &blk_chg);
	if (blk_chg)
		write_block (block, blk);
}

static void
check_zones(unsigned int i) {
	struct minix_inode * inode;

	if (!i || i > INODES)
		return;
	if (inode_count[i] > 1)	/* have we counted this file already? */
		return;
	inode = Inode + i;
	if (!S_ISDIR(inode->i_mode) && !S_ISREG(inode->i_mode) &&
	    !S_ISLNK(inode->i_mode))
		return;
	for (i=0 ; i<7 ; i++)
		add_zone(i + inode->i_zone, &changed);
	add_zone_ind(7 + inode->i_zone, &changed);
	add_zone_dind(8 + inode->i_zone, &changed);
}

static void
check_zones2 (unsigned int i) {
	struct minix2_inode *inode;

	if (!i || i > INODES)
		return;
	if (inode_count[i] > 1)	/* have we counted this file already? */
		return;
	inode = Inode2 + i;
	if (!S_ISDIR (inode->i_mode) && !S_ISREG (inode->i_mode)
	    && !S_ISLNK (inode->i_mode))
		return;
	for (i = 0; i < 7; i++)
		add_zone2 (i + inode->i_zone, &changed);
	add_zone_ind2 (7 + inode->i_zone, &changed);
	add_zone_dind2 (8 + inode->i_zone, &changed);
	add_zone_tind2 (9 + inode->i_zone, &changed);
}

static void
check_file(struct minix_inode * dir, unsigned int offset) {
	static char blk[BLOCK_SIZE];
	struct minix_inode * inode;
	int ino;
	char * name;
	int block;

	block = map_block(dir,offset/BLOCK_SIZE);
	read_block(block, blk);
	name = blk + (offset % BLOCK_SIZE) + 2;
	ino = * (unsigned short *) (name-2);
	if (ino > INODES) {
		get_current_name();
		printf(_("The directory '%s' contains a bad inode number "
			 "for file '%.*s'."),
		       current_name, namelen, name);
		if (ask(_(" Remove"),1)) {
			*(unsigned short *)(name-2) = 0;
			write_block(block, blk);
		}
		ino = 0;
	}	
	if (name_depth < MAX_DEPTH)
		strncpy (name_list[name_depth], name, namelen);
	name_depth++;
	inode = get_inode(ino);
	name_depth--;
	if (!offset) {
		if (!inode || strcmp(".",name)) {
			get_current_name();
			printf(_("%s: bad directory: '.' isn't first\n"),
			       current_name);
			errors_uncorrected = 1;
		} else return;
	}
	if (offset == dirsize) {
		if (!inode || strcmp("..",name)) {
			get_current_name();
			printf(_("%s: bad directory: '..' isn't second\n"),
			       current_name);
			errors_uncorrected = 1;
		} else return;
	}
	if (!inode)
		return;
	if (name_depth < MAX_DEPTH)
		strncpy(name_list[name_depth], name, namelen);
	name_depth++;	
	if (list) {
		if (verbose)
			printf("%6d %07o %3d ", ino,
			       inode->i_mode, inode->i_nlinks);
		get_current_name();
		printf("%s", current_name);
		if (S_ISDIR(inode->i_mode))
			printf(":\n");
		else
			printf("\n");
	}
	check_zones(ino);
	if (inode && S_ISDIR(inode->i_mode))
		recursive_check(ino);
	name_depth--;
	return;
}

static void
check_file2 (struct minix2_inode *dir, unsigned int offset) {
	static char blk[BLOCK_SIZE];
	struct minix2_inode *inode;
	int ino;
	char *name;
	int block;

	block = map_block2 (dir, offset / BLOCK_SIZE);
	read_block (block, blk);
	name = blk + (offset % BLOCK_SIZE) + 2;
	ino = *(unsigned short *) (name - 2);
	if (ino > INODES) {
		get_current_name();
		printf(_("The directory '%s' contains a bad inode number "
			 "for file '%.*s'."),
			  current_name, namelen, name);
		if (ask (_(" Remove"), 1)) {
			*(unsigned short *) (name - 2) = 0;
			write_block (block, blk);
		}
		ino = 0;
	}
	if (name_depth < MAX_DEPTH)
		strncpy (name_list[name_depth], name, namelen);
	name_depth++;
	inode = get_inode2 (ino);
	name_depth--;
	if (!offset) {
		if (!inode || strcmp (".", name)) {
			get_current_name ();
			printf (_("%s: bad directory: '.' isn't first\n"),
				current_name);
			errors_uncorrected = 1;
		} else
			return;
	}
	if (offset == dirsize) {
		if (!inode || strcmp ("..", name)) {
			get_current_name ();
			printf (_("%s: bad directory: '..' isn't second\n"),
				current_name);
			errors_uncorrected = 1;
		} else
			return;
	}
	if (!inode)
		return;
	name_depth++;
	if (list) {
		if (verbose)
			printf ("%6d %07o %3d ", ino, inode->i_mode,
				inode->i_nlinks);
		get_current_name ();
		printf("%s", current_name);
		if (S_ISDIR (inode->i_mode))
			printf (":\n");
		else
			printf ("\n");
	}
	check_zones2 (ino);
	if (inode && S_ISDIR (inode->i_mode))
		recursive_check2 (ino);
	name_depth--;
	return;
}

static void
recursive_check(unsigned int ino) {
	struct minix_inode * dir;
	unsigned int offset;

	dir = Inode + ino;
	if (!S_ISDIR(dir->i_mode))
		die(_("internal error"));
	if (dir->i_size < 2 * dirsize) {
		get_current_name();
		printf(_("%s: bad directory: size < 32"),
		       current_name);
		errors_uncorrected = 1;
	}
	for (offset = 0 ; offset < dir->i_size ; offset += dirsize)
		check_file(dir,offset);
}

static void
recursive_check2 (unsigned int ino) {
	struct minix2_inode *dir;
	unsigned int offset;

	dir = Inode2 + ino;
	if (!S_ISDIR (dir->i_mode))
		die(_("internal error"));
	if (dir->i_size < 2 * dirsize) {
		get_current_name ();
		printf (_("%s: bad directory: size < 32"),
			current_name);
		errors_uncorrected = 1;
	}
	for (offset = 0; offset < dir->i_size; offset += dirsize)
		check_file2 (dir, offset);
}

static int
bad_zone(int i) {
	char buffer[1024];

	if (BLOCK_SIZE*i != lseek(IN, BLOCK_SIZE*i, SEEK_SET))
		die(_("seek failed in bad_zone"));
	return (BLOCK_SIZE != read(IN, buffer, BLOCK_SIZE));
}

static void
check_counts(void) {
	int i;

	for (i=1 ; i <= INODES ; i++) {
		if (!inode_in_use(i) && Inode[i].i_mode && warn_mode) {
			printf(_("Inode %d mode not cleared."),i);
			if (ask(_("Clear"),1)) {
				Inode[i].i_mode = 0;
				changed = 1;
			}
		}
		if (!inode_count[i]) {
			if (!inode_in_use(i))
				continue;
			printf(_("Inode %d not used, marked used in the bitmap."),i);
			if (ask(_("Clear"),1))
				unmark_inode(i);
			continue;
		}
		if (!inode_in_use(i)) {
			printf(_("Inode %d used, marked unused in the bitmap."),
				i);
			if (ask(_("Set"),1))
				mark_inode(i);
		}
		if (Inode[i].i_nlinks != inode_count[i]) {
			printf(_("Inode %d (mode = %07o), i_nlinks=%d, counted=%d."),
				i,Inode[i].i_mode,Inode[i].i_nlinks,inode_count[i]);
			if (ask(_("Set i_nlinks to count"),1)) {
				Inode[i].i_nlinks=inode_count[i];
				changed=1;
			}
		}
	}
	for (i=FIRSTZONE ; i < ZONES ; i++) {
		if (zone_in_use(i) == zone_count[i])
			continue;
		if (!zone_count[i]) {
			if (bad_zone(i))
				continue;
			printf(_("Zone %d: marked in use, no file uses it."),i);
			if (ask(_("Unmark"),1))
				unmark_zone(i);
			continue;
		}
		if (zone_in_use(i))
			printf(_("Zone %d: in use, counted=%d\n"),
			       i, zone_count[i]);
		else
			printf(_("Zone %d: not in use, counted=%d\n"),
			       i, zone_count[i]);
	}
}

static void
check_counts2 (void) {
	int i;

	for (i = 1; i <= INODES; i++) {
		if (!inode_in_use (i) && Inode2[i].i_mode && warn_mode) {
			printf (_("Inode %d mode not cleared."), i);
			if (ask (_("Clear"), 1)) {
				Inode2[i].i_mode = 0;
				changed = 1;
			}
		}
		if (!inode_count[i]) {
			if (!inode_in_use (i))
				continue;
			printf (_("Inode %d not used, marked used in the bitmap."), i);
			if (ask (_("Clear"), 1))
				unmark_inode (i);
			continue;
		}
		if (!inode_in_use (i)) {
			printf (_("Inode %d used, marked unused in the bitmap."), i);
			if (ask (_("Set"), 1))
				mark_inode (i);
		}
		if (Inode2[i].i_nlinks != inode_count[i]) {
			printf (_("Inode %d (mode = %07o), i_nlinks=%d, counted=%d."),
				i, Inode2[i].i_mode, Inode2[i].i_nlinks, inode_count[i]);
			if (ask (_("Set i_nlinks to count"), 1)) {
				Inode2[i].i_nlinks = inode_count[i];
				changed = 1;
			}
		}
	}
	for (i = FIRSTZONE; i < ZONES; i++) {
		if (zone_in_use (i) == zone_count[i])
			continue;
		if (!zone_count[i]) {
			if (bad_zone (i))
				continue;
			printf (_("Zone %d: marked in use, no file uses it."),
				i);
			if (ask (_("Unmark"), 1))
				unmark_zone (i);
			continue;
		}
		if (zone_in_use (i))
			printf (_("Zone %d: in use, counted=%d\n"),
				i, zone_count[i]);
		else
			printf (_("Zone %d: not in use, counted=%d\n"),
				i, zone_count[i]);
	}
}

static void
check(void) {
	memset(inode_count,0,(INODES + 1) * sizeof(*inode_count));
	memset(zone_count,0,ZONES*sizeof(*zone_count));
	check_zones(ROOT_INO);
	recursive_check(ROOT_INO);
	check_counts();
}

static void
check2 (void) {
	memset (inode_count, 0, (INODES + 1) * sizeof (*inode_count));
	memset (zone_count, 0, ZONES * sizeof (*zone_count));
	check_zones2 (ROOT_INO);
	recursive_check2 (ROOT_INO);
	check_counts2 ();
}

int
main(int argc, char ** argv) {
	struct termios tmp;
	int retcode = 0;
	char *p;

	program_name = (argc && *argv) ? argv[0] : "fsck.minix";
	if ((p = strrchr(program_name, '/')) != NULL)
		program_name = p+1;

	if (argc == 2 &&
	    (!strcmp(argv[1], "-V") || !strcmp(argv[1], "--version"))) {
		printf(_("%s (%s)\n"), program_name, PACKAGE_STRING);
		exit(0);
	}

	if (INODE_SIZE * MINIX_INODES_PER_BLOCK != BLOCK_SIZE)
		die(_("bad inode size"));
	if (INODE_SIZE2 * MINIX2_INODES_PER_BLOCK != BLOCK_SIZE)
		die(_("bad v2 inode size"));

	while (argc-- > 1) {
		argv++;
		if (argv[0][0] != '-') {
			if (device_name)
				usage();
			else
				device_name = argv[0];
		} else while (*++argv[0])
			switch (argv[0][0]) {
				case 'l': list=1; break;
				case 'a': automatic=1; repair=1; break;
				case 'r': automatic=0; repair=1; break;
				case 'v': verbose=1; break;
				case 's': show=1; break;
				case 'm': warn_mode=1; break;
				case 'f': force=1; break;
				default: usage();
			}
	}
	if (!device_name)
		usage();
#if LINUX
	check_mount();		/* trying to check a mounted filesystem? */
#endif
	if (repair && !automatic) {
		if (!isatty(0) || !isatty(1))
			die(_("need terminal for interactive repairs"));
	}
	IN = open(device_name,repair?O_RDWR:O_RDONLY);
	if (IN < 0)
		die(_("unable to open '%s': %s"), device_name, strerror(errno));

	/* unnecessary in ELKS build and for speed, remove sync*/
	/***for (count=0 ; count<3 ; count++)
		sync();***/
	read_superblock();

	/*
	 * Determine whether or not we should continue with the checking.
	 * This is based on the status of the filesystem valid and error
	 * flags and whether or not the -f switch was specified on the 
	 * command line.
	 */
	if ( !(Super.s_state & MINIX_ERROR_FS) && 
	      (Super.s_state & MINIX_VALID_FS) && 
	      !force ) {
		if (repair)
			printf(_("%s is clean, no check.\n"), device_name);
		return retcode;
	}
	else if (force)
		printf(_("Forcing filesystem check on %s.\n"), device_name);
	else if (repair)
		printf(_("Filesystem on %s is dirty, needs checking.\n"),\
			device_name);

	read_tables();

	/* Restore the terminal state on fatal signals.
	 * We don't do this for SIGALRM, SIGUSR1 or SIGUSR2.
	 */
	signal(SIGINT, fatalsig);
	signal(SIGQUIT, fatalsig);
	signal(SIGTERM, fatalsig);

	if (repair && !automatic) {
		tcgetattr(0,&termios);
		tmp = termios;
		tmp.c_lflag &= ~(ICANON|ECHO);
		tcsetattr(0,TCSANOW,&tmp);
		termios_set = 1;
	}

	if (version2) {
		check_root2 ();
		check2 ();
	} else {
		check_root();
		check();
	}
	if (verbose) {
		int i, free;

		for (i=1,free=0 ; i <= INODES ; i++)
			if (!inode_in_use(i))
				free++;
		printf(_("\n%6ld inodes used (%2ld%%) %6ld total\n"),(INODES-free),
			100*(INODES-free)/INODES, INODES);
		for (i=FIRSTZONE,free=0 ; i < ZONES ; i++)
			if (!zone_in_use(i))
				free++;
		printf(_("%6ld  zones used (%2ld%%) %6ld total\n"),
			(ZONES-free), 100*(ZONES-free)/ZONES, ZONES);
		printf(_("\n%6d regular files\n"
		"%6d directories\n"
		"%6d character device files\n"
		"%6d block device files\n"
		"%6d links\n"
		"%6d symbolic links\n"
		"------\n"
		"%6d files\n"),
		regular,directory,chardev,blockdev,
		links-2*directory+1,symlinks,total-2*directory+1);
	}
	if (changed) {
		write_tables();
		printf(_(	"----------------------------\n"
			"FILE SYSTEM HAS BEEN CHANGED\n"
			"----------------------------\n"));
		/* unnecessary in ELKS build and for speed, remove sync*/
		/***for (count=0 ; count<3 ; count++)
			sync();***/
	}
	else if ( repair )
		write_super_block();
	
	if (repair && !automatic)
		tcsetattr(0,TCSANOW,&termios);

	if (changed)
	      retcode += 3;
	if (errors_uncorrected)
	      retcode += 4;
	return retcode;
}

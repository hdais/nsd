/*
 * nsd.c -- nsd(8)
 *
 * Copyright (c) 2001-2006, NLnet Labs. All rights reserved.
 *
 * See LICENSE for the license.
 *
 */

#include <config.h>

#include <sys/types.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "nsd.h"
#include "options.h"
#ifdef TSIG
#include "tsig.h"
#endif /* TSIG */

/* The server handler... */
static struct nsd nsd;
static char hostname[MAXHOSTNAMELEN];

static void error(const char *format, ...) ATTR_FORMAT(printf, 1, 2);

static void
usage (void)
{
	fprintf(stderr, "Usage: nsd [OPTION]...\n");
	fprintf(stderr, "Name Server Daemon.\n\n");
	fprintf(stderr,
		"Supported options:\n"
		"  -4              Only listen to IPv4 connections.\n"
		"  -6              Only listen to IPv6 connections.\n"
		"  -a ip-address   Listen to the specified incoming IP address (may be\n"
		"                  specified multiple times).\n"
		"  -c configfile   Read specified configfile instead of %s.\n"
		"  -d              Enable debug mode (do not fork as a daemon process).\n"
#ifndef NDEBUG
		"  -F facilities   Specify the debug facilities.\n"
#endif /* NDEBUG */
		"  -f database     Specify the database to load.\n"
		"  -h              Print this help information.\n"
		, CONFIGFILE);
	fprintf(stderr,
		"  -i identity     Specify the identity when queried for id.server CHAOS TXT.\n"
#ifdef NSID
                "  -I nsid         Specify the NSID. This must be a hex string.\n"
#endif /* NSID */
#ifndef NDEBUG
		"  -L level        Specify the debug level.\n"
#endif /* NDEBUG */
		"  -l filename     Specify the log file.\n"
		"  -N server-count The number of servers to start.\n"
		"  -n tcp-count    The maximum number of TCP connections per server.\n"
		"  -P pidfile      Specify the PID file to write.\n"
		"  -p port         Specify the port to listen to.\n"
		"  -s seconds      Dump statistics every SECONDS seconds.\n"
		"  -t chrootdir    Change root to specified directory on startup.\n"
		);
	fprintf(stderr,
		"  -u user         Change effective uid to the specified user.\n"
		"  -V level        Specify verbosity level.\n"
		"  -v              Print version information.\n"
		);
	fprintf(stderr, "Version %s. Report bugs to <%s>.\n", 
		PACKAGE_VERSION, PACKAGE_BUGREPORT);
}

static void
version(void)
{
	fprintf(stderr, "%s version %s\n", PACKAGE_NAME, PACKAGE_VERSION);
	fprintf(stderr, "Written by NLnet Labs.\n\n");
	fprintf(stderr,
		"Copyright (C) 2001-2006 NLnet Labs.  This is free software.\n"
		"There is NO warranty; not even for MERCHANTABILITY or FITNESS\n"
		"FOR A PARTICULAR PURPOSE.\n");
	exit(0);
}

static void
error(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	log_vmsg(LOG_ERR, format, args);
	va_end(args);
	exit(1);
}

pid_t 
readpid (const char *file)
{
	int fd;
	pid_t pid;
	char pidbuf[16];
	char *t;
	int l;

	if ((fd = open(file, O_RDONLY)) == -1) {
		return -1;
	}

	if (((l = read(fd, pidbuf, sizeof(pidbuf)))) == -1) {
		close(fd);
		return -1;
	}

	close(fd);

	/* Empty pidfile means no pidfile... */
	if (l == 0) {
		errno = ENOENT;
		return -1;
	}

	pid = strtol(pidbuf, &t, 10);

	if (*t && *t != '\n') {
		return -1;
	}
	return pid;
}

int 
writepid (struct nsd *nsd)
{
	FILE * fd;
	char pidbuf[32];

	snprintf(pidbuf, sizeof(pidbuf), "%lu\n", (unsigned long) nsd->pid);

	if ((fd = fopen(nsd->pidfile, "w")) ==  NULL ) {
		return -1;
	}

	if (!write_data(fd, pidbuf, strlen(pidbuf))) {
		fclose(fd);
		return -1;
	}
	fclose(fd);

	if (chown(nsd->pidfile, nsd->uid, nsd->gid) == -1) {
		log_msg(LOG_ERR, "cannot chown %u.%u %s: %s",
			(unsigned) nsd->uid, (unsigned) nsd->gid,
			nsd->pidfile, strerror(errno));
		return -1;
	}

	return 0;
}
	

void 
sig_handler (int sig)
{
	/* To avoid race cond. We really don't want to use log_msg() in this handler */
	
	/* Are we a child server? */
	if (nsd.server_kind != NSD_SERVER_MAIN) {
		switch (sig) {
		case SIGCHLD:
			nsd.signal_hint_child = 1;
			break;
		case SIGALRM:
			break;
		case SIGHUP:
		case SIGINT:
		case SIGTERM:
			nsd.signal_hint_quit = 1;
			break;
		case SIGILL:
		case SIGUSR1:	/* Dump stats on SIGUSR1.  */
			nsd.signal_hint_statsusr = 1;
			break;
		default:
			break;
		}
		return;
	}

	switch (sig) {
	case SIGCHLD:
		nsd.signal_hint_child = 1;
		return;
	case SIGHUP:
		nsd.signal_hint_reload = 1;
		return;
	case SIGALRM:
		nsd.signal_hint_stats = 1;
		break;
	case SIGILL:
		/*
		 * For backwards compatibility with BIND 8 and older
		 * versions of NSD.
		 */
		nsd.signal_hint_statsusr = 1;
		break;
	case SIGUSR1:
		/* Dump statistics.  */
		nsd.signal_hint_statsusr = 1;
		break;
	case SIGINT:
		/* Silent shutdown... */
		nsd.signal_hint_quit = 1;
		break;
	case SIGTERM:
	default:
		nsd.signal_hint_shutdown = 1;
		break;
	}
}

/*
 * Statistic output...
 *
 */
#ifdef BIND8_STATS
void 
bind8_stats (struct nsd *nsd)
{
	char buf[MAXSYSLOGMSGLEN];
	char *msg, *t;
	int i, len;

	/* Current time... */
	time_t now;
	time(&now);

	/* NSTATS */
	t = msg = buf + snprintf(buf, MAXSYSLOGMSGLEN, "NSTATS %lu %lu",
				 (unsigned long) now, (unsigned long) nsd->st.boot);
	for (i = 0; i <= 255; i++) {
		/* How much space left? */
		if ((len = buf + MAXSYSLOGMSGLEN - t) < 32) {
			log_msg(LOG_INFO, "%s", buf);
			t = msg;
			len = buf + MAXSYSLOGMSGLEN - t;
		}

		if (nsd->st.qtype[i] != 0) {
			t += snprintf(t, len, " %s=%lu", rrtype_to_string(i), nsd->st.qtype[i]);
		}
	}
	if (t > msg)
		log_msg(LOG_INFO, "%s", buf);

	/* XSTATS */
	/* Only print it if we're in the main daemon or have anything to report... */
	if (nsd->server_kind == NSD_SERVER_MAIN
	    || nsd->st.dropped || nsd->st.raxfr || (nsd->st.qudp + nsd->st.qudp6 - nsd->st.dropped)
	    || nsd->st.txerr || nsd->st.opcode[OPCODE_QUERY] || nsd->st.opcode[OPCODE_IQUERY]
	    || nsd->st.wrongzone || nsd->st.ctcp + nsd->st.ctcp6 || nsd->st.rcode[RCODE_SERVFAIL]
	    || nsd->st.rcode[RCODE_FORMAT] || nsd->st.nona || nsd->st.rcode[RCODE_NXDOMAIN]
	    || nsd->st.opcode[OPCODE_UPDATE]) {

		log_msg(LOG_INFO, "XSTATS %lu %lu"
			" RR=%lu RNXD=%lu RFwdR=%lu RDupR=%lu RFail=%lu RFErr=%lu RErr=%lu RAXFR=%lu"
			" RLame=%lu ROpts=%lu SSysQ=%lu SAns=%lu SFwdQ=%lu SDupQ=%lu SErr=%lu RQ=%lu"
			" RIQ=%lu RFwdQ=%lu RDupQ=%lu RTCP=%lu SFwdR=%lu SFail=%lu SFErr=%lu SNaAns=%lu"
			" SNXD=%lu RUQ=%lu RURQ=%lu RUXFR=%lu RUUpd=%lu",
			(unsigned long) now, (unsigned long) nsd->st.boot,
			nsd->st.dropped, (unsigned long)0, (unsigned long)0, (unsigned long)0, (unsigned long)0,
			(unsigned long)0, (unsigned long)0, nsd->st.raxfr, (unsigned long)0, (unsigned long)0,
			(unsigned long)0, nsd->st.qudp + nsd->st.qudp6 - nsd->st.dropped, (unsigned long)0,
			(unsigned long)0, nsd->st.txerr,
			nsd->st.opcode[OPCODE_QUERY], nsd->st.opcode[OPCODE_IQUERY], nsd->st.wrongzone,
			(unsigned long)0, nsd->st.ctcp + nsd->st.ctcp6,
			(unsigned long)0, nsd->st.rcode[RCODE_SERVFAIL], nsd->st.rcode[RCODE_FORMAT],
			nsd->st.nona, nsd->st.rcode[RCODE_NXDOMAIN],
			(unsigned long)0, (unsigned long)0, (unsigned long)0, nsd->st.opcode[OPCODE_UPDATE]);
	}

}
#endif /* BIND8_STATS */

extern char *optarg;
extern int optind;

int 
main (int argc, char *argv[])
{
	/* Scratch variables... */
	int c;
	pid_t	oldpid;
	size_t i;
	struct sigaction action;
	
	/* For initialising the address info structures */
	struct addrinfo hints[MAX_INTERFACES];
	const char *nodes[MAX_INTERFACES];
	const char *udp_port = 0;
	const char *tcp_port = 0;

	const char *log_filename = NULL;
	const char *configfile = CONFIGFILE;
	
	log_init("nsd");

	/* Initialize the server handler... */
	memset(&nsd, 0, sizeof(struct nsd));
	nsd.region      = region_create(xalloc, free);
	nsd.dbfile	= 0;
	nsd.pidfile	= 0;
	nsd.server_kind = NSD_SERVER_MAIN;
	
	for (i = 0; i < MAX_INTERFACES; i++) {
		memset(&hints[i], 0, sizeof(hints[i]));
		hints[i].ai_family = DEFAULT_AI_FAMILY;
		hints[i].ai_flags = AI_PASSIVE;
		nodes[i] = NULL;
	}

	nsd.identity	= 0;
	nsd.version	= VERSION;
	nsd.username	= 0;
	nsd.chrootdir	= 0;
	nsd.nsid 	= NULL;
	nsd.nsid_len 	= 0;

	nsd.child_count = 0;
	nsd.maximum_tcp_count = 0;
	nsd.current_tcp_count = 0;
	nsd.grab_ip6_optional = 0;
	
	/* EDNS0 */
	edns_init_data(&nsd.edns_ipv4, EDNS_MAX_MESSAGE_LEN);
#if defined(INET6)
# if defined(IPV6_USE_MIN_MTU)
	edns_init_data(&nsd.edns_ipv6, EDNS_MAX_MESSAGE_LEN);
# else /* !defined(IPV6_USE_MIN_MTU) */
	edns_init_data(&nsd.edns_ipv6, IPV6_MIN_MTU);
# endif
#endif

	/* Set up our default identity to gethostname(2) */
	if (gethostname(hostname, MAXHOSTNAMELEN) == 0) {
		nsd.identity = hostname;
	} else {
		log_msg(LOG_ERR,
			"failed to get the host name: %s - using default identity",
			strerror(errno));
		nsd.identity = IDENTITY;
	}


	/* Parse the command line... */
	while ((c = getopt(argc, argv, "46a:c:df:hi:I:l:N:n:P:p:s:u:t:X:V:v"
#ifndef NDEBUG
		"F:L:"
#endif /* NDEBUG */
		)) != -1) {
		switch (c) {
		case '4':
			for (i = 0; i < MAX_INTERFACES; ++i) {
				hints[i].ai_family = AF_INET;
			}
			break;
		case '6':
#ifdef INET6
			for (i = 0; i < MAX_INTERFACES; ++i) {
				hints[i].ai_family = AF_INET6;
			}
#else /* !INET6 */
			error("IPv6 support not enabled.");
#endif /* !INET6 */
			break;
		case 'a':
			if (nsd.ifs < MAX_INTERFACES) {
				nodes[nsd.ifs] = optarg;
				++nsd.ifs;
			} else {
				error("too many interfaces ('-a') specified.");
			}
			break;
		case 'c':
			configfile = optarg;
			break;
		case 'd':
			nsd.debug = 1;
			break;
		case 'f':
			nsd.dbfile = optarg;
			break;
		case 'h':
			usage();
			exit(0);
		case 'i':
			nsd.identity = optarg;
			break;
		case 'I':
#ifdef NSID
			if (nsd.nsid_len != 0) {
				/* can only be given once */
				break;
			}
			if (strlen(optarg) % 2 != 0) {
				error("the NSID must be a hex string of an even length.");
			}
			nsd.nsid = xalloc(strlen(optarg) / 2);
			nsd.nsid_len = strlen(optarg) / 2;
			if (hex_pton(optarg, nsd.nsid, nsd.nsid_len) == -1) {
				error("hex string cannot be parsed '%s' in NSID.", optarg);
			}
#endif /* NSID */
                       break;
		case 'l':
			log_filename = optarg;
			break;
		case 'N':
			i = atoi(optarg);
			if (i <= 0) {
				error("number of child servers must be greather than zero.");
			} else {
				nsd.child_count = i;
			}
			break;
		case 'n':
			i = atoi(optarg);
			if (i <= 0) {
				error("number of concurrent TCP connections must greater than zero.");
			} else {
				nsd.maximum_tcp_count = i;
			}
			break;
		case 'P':
			nsd.pidfile = optarg;
			break;
		case 'p':
			if (atoi(optarg) == 0) {
				error("port argument must be numeric.");
			}
			tcp_port = optarg;
			udp_port = optarg;
			break;
		case 's':
#ifdef BIND8_STATS
			nsd.st.period = atoi(optarg);
#else /* !BIND8_STATS */
			error("BIND 8 statistics not enabled.");
#endif /* !BIND8_STATS */
			break;
		case 't':
#ifdef HAVE_CHROOT
			nsd.chrootdir = optarg;
#else /* !HAVE_CHROOT */
			error("chroot not supported on this platform.");
#endif /* !HAVE_CHROOT */
			break;
		case 'u':
			nsd.username = optarg;
			break;
		case 'V':
			verbosity = atoi(optarg);
			break;
		case 'v':
			version();
			/* version exits */
#ifndef NDEBUG
		case 'F':
			sscanf(optarg, "%x", &nsd_debug_facilities);
			break;
		case 'L':
			sscanf(optarg, "%d", &nsd_debug_level);
			break;
#endif /* NDEBUG */
		case '?':
		default:
			usage();
			exit(1);
		}
	}
	argc -= optind;
	argv += optind;

	if (argc != 0) {
		usage();
		exit(1);
	}

	if (strlen(nsd.identity) > UCHAR_MAX) {
		error("server identity too long (%u characters)",
		      (unsigned) strlen(nsd.identity));
	}

    /* Read options */
	nsd.options = nsd_options_create(region_create(xalloc, free));
	if(!parse_options_file(nsd.options, configfile)) {
		error("nsd: could not read config: %s\n", configfile);
	}
	if(nsd.options->ip4_only) {
		for (i = 0; i < MAX_INTERFACES; ++i) {
			hints[i].ai_family = AF_INET;
		}
	}
#ifdef INET6
	if(nsd.options->ip6_only) {
		for (i = 0; i < MAX_INTERFACES; ++i) {
			hints[i].ai_family = AF_INET6;
		}
	}
#endif /* !INET6 */
	if(nsd.options->ip_addresses)
	{
		ip_address_option_t* ip = nsd.options->ip_addresses;
		while(ip) {
			if (nsd.ifs < MAX_INTERFACES) {
				nodes[nsd.ifs] = ip->address;
				++nsd.ifs;
			} else {
				error("too many interfaces ('-a' + 'ip-address:') specified.");
				break;
			}
			ip = ip->next;
		}
	}
	if (verbosity == 0)
		verbosity = nsd.options->verbosity;
#ifndef NDEBUG
	if (nsd_debug_level > 0 && verbosity == 0)
		verbosity = nsd_debug_level;
#endif /* NDEBUG */
	if(nsd.options->debug_mode) nsd.debug=1;
	if(!nsd.dbfile)
	{
		if(nsd.options->database) nsd.dbfile = nsd.options->database;
		else nsd.dbfile = DBFILE;
	}
	if(!nsd.pidfile)
	{
		if(nsd.options->pidfile) nsd.pidfile = nsd.options->pidfile;
		else nsd.pidfile = PIDFILE;
	}
	if(strcmp(nsd.identity, hostname)==0 || strcmp(nsd.identity,IDENTITY)==0)
	{
		if(nsd.options->identity) nsd.identity = nsd.options->identity;
	}
	if (nsd.options->logfile && !log_filename) {
		log_filename = nsd.options->logfile;
	}
	if(nsd.child_count == 0) {
		nsd.child_count = nsd.options->server_count;
	}
	if(nsd.maximum_tcp_count == 0) {
		nsd.maximum_tcp_count = nsd.options->tcp_count;
	}
	if(udp_port == 0)
	{
		if(nsd.options->port != 0) {
			udp_port = nsd.options->port;
			tcp_port = nsd.options->port;
		} else {
			udp_port = UDP_PORT;
			tcp_port = TCP_PORT;
		}
	}
#ifdef BIND8_STATS
	if(nsd.st.period == 0) {
		nsd.st.period = nsd.options->statistics;
	}
#endif /* !BIND8_STATS */
#ifdef HAVE_CHROOT
	if(nsd.chrootdir == 0) nsd.chrootdir = nsd.options->chroot;
#endif /* HAVE_CHROOT */
	if(nsd.username == 0) {
		if(nsd.options->username) nsd.username = nsd.options->username;
		else nsd.username = USER;
	}
	if(nsd.options->zonesdir && nsd.options->zonesdir[0]) {
		if(chdir(nsd.options->zonesdir)) {
			error("cannot chdir to '%s': %s", 
				nsd.options->zonesdir, strerror(errno));
		}
		DEBUG(DEBUG_IPC,1, (LOG_INFO, "changed directory to %s", 
			nsd.options->zonesdir));
	}
	/* get it from the config file */
#ifdef NSID
	edns_init_nsid(&nsd.edns_ipv4, nsd.nsid_len);
#if defined(INET6)
	edns_init_nsid(&nsd.edns_ipv6, nsd.nsid_len);
#endif /* defined(INET6) */
#endif /* NSID */
	/* Number of child servers to fork.  */
	nsd.children = (struct nsd_child *) region_alloc(
		nsd.region, nsd.child_count * sizeof(struct nsd_child));
	for (i = 0; i < nsd.child_count; ++i) {
		nsd.children[i].kind = NSD_SERVER_BOTH;
		nsd.children[i].pid = -1;
		nsd.children[i].child_fd = -1;
		nsd.children[i].parent_fd = -1;
		nsd.children[i].handler = NULL;
		nsd.children[i].need_to_send_STATS = 0;
		nsd.children[i].need_to_send_QUIT = 0;
		nsd.children[i].need_to_exit = 0;
		nsd.children[i].has_exited = 0;
		nsd.children[i].dirty_zones = stack_create(nsd.region, 
			nsd_options_num_zones(nsd.options));
	}

	nsd.this_child = NULL;
	
	/* We need at least one active interface */
	if (nsd.ifs == 0) {
		nsd.ifs = 1;

		/*
		 * With IPv6 we'd like to open two separate sockets,
		 * one for IPv4 and one for IPv6, both listening to
		 * the wildcard address (unless the -4 or -6 flags are
		 * specified).
		 *
		 * However, this is only supported on platforms where
		 * we can turn the socket option IPV6_V6ONLY _on_.
		 * Otherwise we just listen to a single IPv6 socket
		 * and any incoming IPv4 connections will be
		 * automatically mapped to our IPv6 socket.
		 */
#ifdef INET6
		if (hints[0].ai_family == AF_UNSPEC) {
# ifdef IPV6_V6ONLY
			hints[0].ai_family = AF_INET6;
			hints[1].ai_family = AF_INET;
			nsd.ifs = 2;
			nsd.grab_ip6_optional = 1;
# else /* !IPV6_V6ONLY */
			hints[0].ai_family = AF_INET6;
# endif	/* !IPV6_V6ONLY */
		}
#endif /* INET6 */
	}

	/* Set up the address info structures with real interface/port data */
	for (i = 0; i < nsd.ifs; ++i) {
		int r;

		/* We don't perform name-lookups */
		if (nodes[i] != NULL)
			hints[i].ai_flags |= AI_NUMERICHOST;
		
		hints[i].ai_socktype = SOCK_DGRAM;
		if ((r=getaddrinfo(nodes[i], udp_port, &hints[i], &nsd.udp[i].addr)) != 0) {
#ifdef INET6
			if(nsd.grab_ip6_optional && hints[0].ai_family == AF_INET6) {
				log_msg(LOG_WARNING, "No IPv6, fallback to IPv4. getaddrinfo: %s", 
				r==EAI_SYSTEM?strerror(errno):gai_strerror(r));
				continue;
			}
#endif
			error("cannot parse address '%s': getaddrinfo: %s %s",
				nodes[i]?nodes[i]:"(null)",
				gai_strerror(r),
				r==EAI_SYSTEM?strerror(errno):"");
		}
		
		hints[i].ai_socktype = SOCK_STREAM;
		if ((r=getaddrinfo(nodes[i], tcp_port, &hints[i], &nsd.tcp[i].addr)) != 0) {
			error("cannot parse address '%s': getaddrinfo: %s %s",
				nodes[i]?nodes[i]:"(null)",
				gai_strerror(r),
				r==EAI_SYSTEM?strerror(errno):"");
		}
	}

	/* Parse the username into uid and gid */
	nsd.gid = getgid();
	nsd.uid = getuid();
	if (*nsd.username) {
		struct passwd *pwd;
		if (isdigit(*nsd.username)) {
			char *t;
			nsd.uid = strtol(nsd.username, &t, 10);
			if (*t != 0) {
				if (*t != '.' || !isdigit(*++t)) {
					error("-u user or -u uid or -u uid.gid");
				}
				nsd.gid = strtol(t, &t, 10);
			} else {
				/* Lookup the group id in /etc/passwd */
				if ((pwd = getpwuid(nsd.uid)) == NULL) {
					error("user id %u does not exist.", (unsigned) nsd.uid);
				} else {
					nsd.gid = pwd->pw_gid;
				}
				endpwent();
			}
		} else {
			/* Lookup the user id in /etc/passwd */
			if ((pwd = getpwnam(nsd.username)) == NULL) {
				error("user '%s' does not exist.", nsd.username);
			} else {
				nsd.uid = pwd->pw_uid;
				nsd.gid = pwd->pw_gid;
			}
			endpwent();
		}
	}

#ifdef TSIG
	if(!tsig_init(nsd.region))
		error("init tsig failed");
	key_options_tsig_add(nsd.options);
#endif /* TSIG */

	/* Set up the logging... */
	log_open(LOG_PID, FACILITY, log_filename);
	if (!log_filename) {
		log_set_log_function(log_syslog);
	}
	
	/* Relativize the pathnames for chroot... */
	if (nsd.chrootdir) {
		int l = strlen(nsd.chrootdir);

		/* existing chrootdir: append trailing slash for strncmp checking */
		if (l>0 && strncmp(nsd.chrootdir + (l-1), "/", 1) != 0) {
			char *chroot_slash = region_alloc(nsd.region, sizeof(char)*(l+2)); 
			memcpy(chroot_slash, nsd.chrootdir, sizeof(char)*(l+1)); 
			strncat(chroot_slash, "/", 1);
			nsd.chrootdir = chroot_slash; 
			++l;
		}

		if (strncmp(nsd.chrootdir, nsd.pidfile, l) != 0) {
			log_msg(LOG_ERR, "%s is not relative to %s: will not chroot",
				nsd.pidfile, nsd.chrootdir);
			nsd.chrootdir = NULL;
		} else if (strncmp(nsd.chrootdir, nsd.dbfile, l) != 0) {
			log_msg(LOG_ERR, "%s is not relative to %s: will not chroot",
				nsd.dbfile, nsd.chrootdir);
			nsd.chrootdir = NULL;
		} else if (strncmp(nsd.chrootdir, nsd.options->xfrdfile, l) != 0) {
			log_msg(LOG_ERR, "%s is not relative to %s: will not chroot",
				nsd.options->xfrdfile, nsd.chrootdir);
			nsd.chrootdir = NULL;
		} else if (strncmp(nsd.chrootdir, nsd.options->difffile, l) != 0) {
			log_msg(LOG_ERR, "%s is not relative to %s: will not chroot",
				nsd.options->difffile, nsd.chrootdir);
			nsd.chrootdir = NULL;
		}
	}

	/* Do we have a running nsd? */
	if ((oldpid = readpid(nsd.pidfile)) == -1) {
		if (errno != ENOENT) {
			log_msg(LOG_ERR, "can't read pidfile %s: %s",
				nsd.pidfile, strerror(errno));
		}
	} else {
		if (kill(oldpid, 0) == 0 || errno == EPERM) {
			log_msg(LOG_WARNING,
				"nsd is already running as %u, continuing",
				(unsigned) oldpid);
		} else {
			log_msg(LOG_ERR,
				"...stale pid file from process %u",
				(unsigned) oldpid);
		}
	}

	/* Unless we're debugging, fork... */
	if (!nsd.debug) {
		int fd;

		/* Take off... */
		switch ((nsd.pid = fork())) {
		case 0:
			break;
		case -1:
			log_msg(LOG_ERR, "fork failed: %s", strerror(errno));
			unlink(nsd.pidfile);
			exit(1);
		default:
			exit(0);
		}

		/* Detach ourselves... */
		if (setsid() == -1) {
			log_msg(LOG_ERR, "setsid() failed: %s", strerror(errno));
			exit(1);
		}

		if ((fd = open("/dev/null", O_RDWR, 0)) != -1) {
			(void)dup2(fd, STDIN_FILENO);
			(void)dup2(fd, STDOUT_FILENO);
			(void)dup2(fd, STDERR_FILENO);
			if (fd > 2)
				(void)close(fd);
		}
	}

	/* Setup the signal handling... */
	action.sa_handler = sig_handler;
	sigfillset(&action.sa_mask);
	action.sa_flags = 0;
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGHUP, &action, NULL);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGILL, &action, NULL);
	sigaction(SIGUSR1, &action, NULL);
	sigaction(SIGALRM, &action, NULL);
	sigaction(SIGCHLD, &action, NULL);
	action.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &action, NULL);

	/* Get our process id */
	nsd.pid = getpid();

	/* Initialize... */
	nsd.mode = NSD_RUN;
	nsd.signal_hint_child = 0;
	nsd.signal_hint_reload = 0;
	nsd.signal_hint_quit = 0;
	nsd.signal_hint_shutdown = 0;
	nsd.signal_hint_stats = 0;
	nsd.signal_hint_statsusr = 0;
	nsd.quit_sync_done = 0;

	/* Run the server... */
	if (server_init(&nsd) != 0) {
		DEBUG(DEBUG_IPC,2, (LOG_INFO, "server initialization failed"));
		/* do not tamper with pidfile */
		exit(1);
	}

	/* Overwrite pid after initialization succeeds */
	if (writepid(&nsd) == -1) {
		log_msg(LOG_ERR, "cannot overwrite the pidfile %s: %s",
			nsd.pidfile, strerror(errno));
	}

	log_msg(LOG_NOTICE, "nsd started (%s), pid %d", PACKAGE_STRING, 
		(int) nsd.pid);

	if (nsd.server_kind == NSD_SERVER_MAIN) {
		server_main(&nsd);
	} else {
		server_child(&nsd);
	}

	/* NOTREACH */
	exit(0);
}

//    IPscan - an http-initiated IPv6 port scanner.
//
//    Copyright (C) 2011-2016 Tim Chappell.
//
//    This file is part of IPscan.
//
//    IPscan is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 3 of the License, or
//    (at your option) any later version.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with IPscan.  If not, see <http://www.gnu.org/licenses/>.

// ipscan.c version
// 0.01 - first released version
// 0.02 - additional DEBUG added for MySQL investigation
// 0.03 - added syslog support
// 0.04 - improved HTML (transition to styles, general compliance)
// 0.05 - addition of ICMPv6 ECHO-REQUEST functionality
// 0.06 - removal of empty HTML paragraph
// 0.07 - further buffer overflow prevention measures
// 0.08 - correct printf cast
// 0.09 - tidy up exit calls and verbosity support
// 0.10 - minor include correction for FreeBSD support
// 0.11 - add parallel port scan function
// 0.12 - remove unused parameters
// 0.13 - specifically count number of customport parameters
// 0.14 - add service names to results table (modification to portlist, now structure)
// 0.15 - fix length of requestmethod to prevent potential overflow
// 0.16 - add UDP port scan support
// 0.17 - add parallel UDP port scan support
// 0.18 - separate UDP and TCP debug logging
// 0.19 - added missing log prefix
// 0.20 - add scan automation help when offered a bad query string
// 0.21 - add support for removal of ping
// 0.22 - add support for removal of UDP
// 0.23 - add support for special test cases
// 0.24 - improve special test case debug logging
// 0.25 - add support for test completion reporting
// 0.25 - fix special case handling for custom ports
// 0.26 - correct fetch tidy-up reporting
// 0.27 - update to support further completion report types
// 0.28 - improved error logging
// 0.29 - use random(ish) sessions rather than getpid
// 0.30 - move to use strnlen() in getenv lookups
// 0.31 - improved querystring parsing, truncated session id
// 0.32 - add Navigate away detection
// 0.33 - add reporting for fork() issues
// 0.34 - add automated results deletion for javascript clients
// 0.35 - add support for deletion of orphaned results
// 0.36 - add time() response checks

#include "ipscan.h"
#include "ipscan_portlist.h"
//
#include <stdlib.h>
#include <strings.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <time.h>
#include <inttypes.h>
// toupper/tolower routines
#include <ctype.h>

// Others that FreeBSD highlighted
#include <netinet/in.h>

// IPv6 address conversion
#include <arpa/inet.h>

// String comparison
#include <string.h>

// errors
#include <errno.h>

// Logging with syslog requires additional include
#if (LOGMODE == 1)
	#include <syslog.h>
#endif

// Parallel processing related
#include <sys/wait.h>

//
// Prototype declarations
//

int write_db(uint64_t host_msb, uint64_t host_lsb, uint64_t timestamp, uint64_t session, uint32_t port, int32_t result , char *indirecthost);
int dump_db(uint64_t host_msb, uint64_t host_lsb, uint64_t timestamp, uint64_t session);
int read_db_result(uint64_t host_msb, uint64_t host_lsb, uint64_t timestamp, uint64_t session, uint32_t port);
int delete_from_db(uint64_t host_msb, uint64_t host_lsb, uint64_t timestamp, uint64_t session);
int tidy_up_db(uint64_t time_now);

int check_udp_ports_parll(char * hostname, unsigned int portindex, unsigned int todo, uint64_t host_msb, uint64_t host_lsb, uint64_t timestamp, uint64_t session, struct portlist_struc *udpportlist);
int check_tcp_ports_parll(char * hostname, unsigned int portindex, unsigned int todo, uint64_t host_msb, uint64_t host_lsb, uint64_t timestamp, uint64_t session, struct portlist_struc *portlist);

void create_html_common_header(void);
void create_json_header(void);
void create_html_header(uint64_t session, time_t timestamp, uint16_t numports, uint16_t numudpports, char * reconquery);

void create_html_body(char * hostname, time_t timestamp, uint16_t numports, uint16_t numudpports, struct portlist_struc *portlist, struct portlist_struc *udpportlist);
void create_html_body_end(void);
void create_html_form(uint16_t numports, uint16_t numudpports, struct portlist_struc *portlist, struct portlist_struc *udpportlist);

// create_results_key_table is only referenced if creating the text-only version of the scanner
#if (TEXTMODE == 1)
void create_results_key_table(char * hostname, time_t timestamp);
#endif

// summarise_db is only referenced if summary reporting is enabled
#if (SUMMARYENABLE == 1)
int summarise_db(void);
#endif

// Only include reference to ping-test function if compiled in
#if (IPSCAN_INCLUDE_PING ==1)
int check_icmpv6_echoresponse(char * hostname, uint64_t starttime, uint64_t session, char * router);
#endif



//
// End of prototypes declarations
//

// structure holding the potential results table - entries MUST be in montonically increasing enumerated returnval order
struct rslt_struc resultsstruct[] =
{
	/* returnval,		connrc,	conn_errno		TEXT lbl			TEXT col	Description/User feedback	*/
	{ PORTOPEN, 		0, 		0,	 			"OPEN", 			"red",		"An IPv6 TCP connection was successfully established to this port. You should check that this is the expected outcome since an attacker may be able to compromise your machine by accessing this IPv6 address/port combination."},
	{ PORTABORT, 		-1, 	ECONNABORTED, 	"ABRT", 			"yellow",	"An abort indication was received when attempting to open this port. Someone can ascertain that your machine is responding on this IPv6 address/port combination, but cannot establish a TCP connection."},
	{ PORTREFUSED, 		-1, 	ECONNREFUSED, 	"RFSD", 			"yellow",	"A refused indication (TCP RST/ACK or ICMPv6 type 1 code 4) was received when attempting to open this port. Someone can ascertain that your machine is responding on this IPv6 address/port combination, but cannot establish a TCP connection."},
	{ PORTCRESET, 		-1, 	ECONNRESET, 	"CRST", 			"yellow",	"A connection reset request was received when attempting to open this port. Someone can ascertain that your machine is responding on this IPv6 address/port combination, but cannot establish a TCP connection."},
	{ PORTNRESET, 		-1, 	ENETRESET, 		"NRST", 			"yellow",	"A network reset request was received when attempting to open this port. Someone can ascertain that your machine is responding on this IPv6 address/port combination, but cannot establish a TCP connection."},
	{ PORTINPROGRESS, 	-1, 	EINPROGRESS, 	"STLTH", 			"green",	"No response was received from your machine in the allocated time period. This is the ideal response since no-one can ascertain your machines' presence at this IPv6 address/port combination."},
	{ PORTPROHIBITED, 	-1, 	EACCES, 		"PHBTD", 			"yellow",	"An administratively prohibited response (ICMPv6 type 1 code 1) was received when attempting to open this port. Someone can ascertain that your machine is responding on this IPv6 address/port combination, but cannot establish a TCP connection."},
	{ PORTUNREACHABLE, 	-1, 	ENETUNREACH, 	"NUNRCH", 			"yellow",	"An unreachable response (ICMPv6 type 1 code 0) was received when attempting to open this port. Someone can ascertain that your machine is responding on this IPv6 address/port combination, but cannot establish a TCP connection."},
	{ PORTNOROUTE, 		-1, 	EHOSTUNREACH, 	"HUNRCH", 			"yellow",	"A No route to host response (ICMPv6 type 1 code 3 or ICMPv6 type 3) was received when attempting to open this port. Someone can ascertain that your machine is responding on this IPv6 address/port combination, but cannot establish a TCP connection."},
	{ PORTPKTTOOBIG, 	-1, 	EMSGSIZE, 		"TOOBIG", 			"yellow",	"A Packet too big response (ICMPv6 type 2) was received when attempting to open this port. Someone can ascertain that your machine is responding on this IPv6 address/port combination, but cannot establish a TCP connection."},
	{ PORTPARAMPROB, 	-1, 	EPROTO, 		"PRMPRB", 			"yellow",	"A Parameter problem response (ICMPv6 type 4) was received when attempting to open this port. Someone can ascertain that your machine is responding on this IPv6 address/port combination, but cannot establish a TCP connection."},
	{ ECHONOREPLY, 		-96, 	-96,	 		"ECHO NO REPLY",	"green",	"No ICMPv6 ECHO_REPLY packet was received in response to the ICMPv6 ECHO_REQUEST which was sent. This is the ideal response since no-one can ascertain your machines' presence at this IPv6 address."},
	{ ECHOREPLY, 		-97, 	-97,	 		"ECHO REPLY", 		"yellow",	"An ICMPv6 ECHO_REPLY packet was received in response to the ICMPv6 ECHO_REQUEST which was sent. Someone can ascertain that your machine is present on this IPv6 address."},
	{ UDPOPEN,			-95,	-95,			"UDPOPEN",			"red",		"A valid response was received from this UDP port. You should check that this is the expected outcome since an attacker may be able to compromise your machine by accessing this IPv6 address/port combination."},
	{ UDPSTEALTH,		-1,		EAGAIN,			"UDPSTEALTH",		"green",	"No UDP response was received from your machine in the allocated time period. This is the ideal response since no-one can ascertain your machines' presence at this IPv6 address/port combination."},
	/* Unexpected and unknown error response cases, do NOT change */
	{ PORTUNEXPECTED,	-98,	-98,			"UNXPCT",			"white",	"An unexpected response was received to the connect attempt."},
	{ PORTUNKNOWN, 		-99,	-99, 			"UNKWN", 			"white",	"An unknown error response was received, or the port is yet to be tested."},
	{ PORTINTERROR,		-100,	-100,			"INTERR",			"white",	"An internal error occurred."},
	/* End of list marker, do NOT change */
	{ PORTEOL,			-101,	-101,			"EOL",				"black",	"End of list marker."}
};


uint64_t get_session(void)
{
	uint64_t sessionnum = 0;
	uint64_t fetchedsession = 0;
	FILE *fp;
	fp = fopen("/dev/urandom", "r");

	if (NULL == fp)
	{
		sessionnum = (uint64_t)getpid();
		IPSCAN_LOG( LOGPREFIX "ipscan: ERROR : Cannot open /dev/urandom, %d (%s), defaulting session to getpid() = %"PRIu64"\n", errno, strerror(errno), sessionnum);
	}
	else
	{
		size_t numitems = fread( &fetchedsession, sizeof(fetchedsession), 1, fp);
		fclose(fp);
		// Clear the MSB of the random session ID so that we're sure it will fit into an int64_t which the QUERY_STRING parser assumes
		sessionnum = fetchedsession & ( ((uint64_t)~0) >> 1);

		#ifdef QUERYDEBUG
		IPSCAN_LOG( LOGPREFIX "ipscan: Session number modification check, before = %"PRIu64" after = %"PRIu64"\n", fetchedsession, sessionnum);
		#endif

		if (1 != numitems)
		{
			sessionnum = (uint64_t)getpid();
			IPSCAN_LOG( LOGPREFIX "ipscan: ERROR : Cannot read /dev/urandom, defaulting session to getpid() = %"PRIu64"\n", sessionnum);
		}
	}
	return (sessionnum);
}

int main(void)
{

	#if (TEXTMODE == 1)
	// last is only used in text-only mode
	int last = 0;
	#else
	int fetchnum = 0;
	#endif

	// List of ports to be tested and their results
	struct portlist_struc portlist[MAXPORTS];

	int result;

	#if (TEXTMODE != 1)
	// Default for unused database entries
	char unusedfield[8] = "unused\0";
	#endif

	// Only necessary if we're including ping support
	#if (IPSCAN_INCLUDE_PING == 1)
	int pingresult;
	// Storage for indirecthost address, in case required
	char indirecthost[INET6_ADDRSTRLEN];
	#endif

	char remoteaddrstring[INET6_ADDRSTRLEN];
	char *remoteaddrvar;

	unsigned int position = 0;

	// Default to testing
	int beginscan = 0;
	int fetch = 0;

	// the session starttime, used as an unique index for the database
	time_t   starttime;
	// the query derived starttime
	int64_t  querystarttime;

	uint8_t special;
	uint16_t port;
	uint16_t portindex;

	// Parallel scanning related
	int numchildren;
	int remaining;
	int childstatus;
	int porti;

	// Ports to be tested
	uint16_t numports = 0;
	uint16_t numudpports = NUMUDPPORTS;

	// "general purpose" variables, used as required
	int rc = 0;
	unsigned int i = 0;
	unsigned int shift = 0;
	unsigned int j = 0;

	// stats
	unsigned int portsstats[ NUMRESULTTYPES ];

	// Determine request method and query-string
	char requestmethod[ (MAXREQMETHODLEN + 1) ];
	char thischar;
	char *reqmethodvar;
	char *querystringvar;
	char querystring[ (MAXQUERYSTRLEN + 1) ];


	// buffer for reconstituted querystring
	int reconquerysize = MAXQUERYSTRLEN;
	char reconquery[ (MAXQUERYSTRLEN + 1) ];
	char *reconptr = &reconquery[0];

	// buffer for logging entries
	int logbuffersize = LOGENTRYLEN;
	char logbuffer[ (LOGENTRYLEN + 1) ];
	char *logbufferptr = &logbuffer[0];

	// Structure to hold querystring variable names, their values and a validity indication
	typedef struct {
		char varname[(MAXQUERYNAMELEN+2)];
		int64_t varval; // Signed since some values will be negative
		int valid;
	} queries;

	queries query[MAXQUERIES];
	unsigned int numqueries = 0;
	int64_t varval = 0; // temporary storage for query string parameters
	// value string - add two chars to cope with trailing \0
	char valstring[ (MAXQUERYVALLEN + 2) ];
	
	int64_t magic = 0;
	int includeexisting = 0;

	// IPv6 address related
	unsigned char remotehost[sizeof(struct in6_addr)];

	uint64_t value;
	uint64_t remotehost_msb = 0;
	uint64_t remotehost_lsb = 0;

	// If syslog is in use then open the log
	#if (LOGMODE == 1)
		openlog("ipscan", LOG_PID, LOG_LOCAL0);
	#endif

	// Initialise the port list
	for (i = 0; i < DEFNUMPORTS; i++)
	{
		portlist[i] = defportlist[i];
	}

	// Clear out the port result type statistics
	for (i = 0 ; i < NUMRESULTTYPES ; i++)
	{
		portsstats[i] = 0;
	}

	// Process id related - this version is extracted from the querystring
	// note: this is signed, whereas original value is a suitably truncated unsigned integer
	int64_t querysession = 0;

	// Log the current time and "session" with which to initiate scan and fetch results
	// These should ensure that each test is globally unique when client IP address is also used.
	starttime = time(0);
	if (starttime < 0)
	{
		IPSCAN_LOG( LOGPREFIX "ipscan: ERROR: time() returned bad value for starttime %d (%s)\n", errno, strerror(errno));
	}
	uint64_t session = get_session();

	// Tidy up the database - really only required for orphaned results
	// e.g. caused by server shutdown whilst a scan is in progress
	//
	if (starttime > 0)
	{
		rc = tidy_up_db( (uint64_t)starttime );
		if (0 != rc) IPSCAN_LOG( LOGPREFIX "ipscan: ERROR: tidy_up_db() returned %d\n", rc);
	}

	// QUERY_STRING / REQUEST_METHOD
	// URL is of the form: ipv6.cgi?name1=value1&name2=value2
	// REQUEST_METHOD = GET
	// QUERY_STRING = name1=value1&name2=value2 
	reqmethodvar = getenv("REQUEST_METHOD");
	querystringvar = getenv("QUERY_STRING");

	// ensure length OK
	if (NULL == reqmethodvar)
	{
		IPSCAN_LOG( LOGPREFIX "ipscan: ERROR : REQUEST_METHOD variable lookup returned NULL.");
	}
	else if ( strnlen(reqmethodvar, (MAXREQMETHODLEN+1)) > MAXREQMETHODLEN )
	{
		IPSCAN_LOG( LOGPREFIX "ipscan: ATTACK?: REQUEST_METHOD variable string is longer than allocated buffer (%d > %d)\n", (int)strnlen(reqmethodvar, (MAXREQMETHODLEN+1)), MAXREQMETHODLEN);
		// Create the header
		create_html_common_header();
		// Now finish the header
		printf("<TITLE>IPv6 Port Scanner Version %s</TITLE>\n", IPSCAN_VER);
		printf("</HEAD>\n");
		printf("<BODY>\n");
		printf("<P>I was called with REQUEST_METHOD longer than my allocated buffer. That is very disappointing.</P>\n");
		// Finish the html
		create_html_body_end();
		exit(EXIT_FAILURE);
	}
	else if( sscanf(reqmethodvar,"%"TO_STR(MAXREQMETHODLEN)"s",requestmethod) != 1 )
	{
		IPSCAN_LOG( LOGPREFIX "ipscan: Invalid request-method scan.");
	}
	else
	{
		#ifdef QUERYDEBUG
		IPSCAN_LOG( LOGPREFIX "ipscan: Request method is : %s\n", requestmethod);
		#endif

		// Force Uppercase to ease comparison
		for (i = 0; i < strnlen(requestmethod, (MAXREQMETHODLEN+1)); i++)
		{
			thischar=requestmethod[i];
			requestmethod[i]=toupper(thischar);
		}

		if (strncmp("GET", requestmethod, 3) == 0)
		{
			if(NULL == querystringvar)
			{
				IPSCAN_LOG( LOGPREFIX "ipscan: ERROR: QUERY_STRING variable lookup returned NULL.\n");
			}
			else if ( strnlen(querystringvar, MAXQUERYSTRLEN+1) > MAXQUERYSTRLEN)
			{
				IPSCAN_LOG( LOGPREFIX "ipscan: ATTACK?: QUERY_STRING environment string is longer than allocated buffer (%d > %d)\n", (int)strnlen(querystringvar, MAXQUERYSTRLEN+1), MAXQUERYSTRLEN);
				// Create the header
				create_html_common_header();
				// Now finish the header
				printf("<TITLE>IPv6 Port Scanner Version %s</TITLE>\n", IPSCAN_VER);
				printf("</HEAD>\n");
				printf("<BODY>\n");
				printf("<P>I was called with a QUERY_STRING longer than my allocated buffer. That is very disappointing.</P>\n");
				// Finish the html
				create_html_body_end();
				exit(EXIT_FAILURE);
			}
			else if( sscanf(querystringvar,"%"TO_STR(MAXQUERYSTRLEN)"s",querystring) != 1 )
			{
				#ifdef QUERYDEBUG
				// No query string will get reported here ....
				IPSCAN_LOG( LOGPREFIX "ipscan: Invalid query-string sscanf.\n");
				#endif
			}
			else
			{
				#ifdef QUERYDEBUG
				IPSCAN_LOG( LOGPREFIX "ipscan: DEBUG info: Query-string : %s\n", querystring);
				#endif


				// Force lowercase to ease later comparison
				for (i = 0; i < strnlen(querystring,(MAXQUERYSTRLEN)); i++)
				{
					thischar=querystring[i];
					querystring[i]=tolower(thischar);
				}

				//
				// Split the query string into variable names and values
				//
				// URL is of the form: ipscan-js.cgi?name1=value1&name2=value2
				unsigned int queryindex = 0;
				int finished = 0;

				while (queryindex < MAXQUERYSTRLEN && querystring[queryindex] >= 32 && finished == 0 && numqueries < MAXQUERIES)
				{
					int varnameindex = 0;
					query[numqueries].valid = 0;
					while ( querystring[queryindex] >= 32 && querystring[queryindex] < 127 && querystring[queryindex] != '='
						&& querystring[queryindex] != '&' && queryindex < MAXQUERYSTRLEN && varnameindex < MAXQUERYNAMELEN && finished == 0)
					{
						query[numqueries].varname[varnameindex] = querystring[queryindex];
						varnameindex ++;
						queryindex ++;
					}
					if (varnameindex >= MAXQUERYNAMELEN)
					{
						IPSCAN_LOG( LOGPREFIX "ipscan: query parameter name string is too long : %s\n", querystring);
						varnameindex = MAXQUERYNAMELEN; // Truncate
					}
					query[numqueries].varname[varnameindex]=0; // Add termination

					finished = (querystring[queryindex] < 32 || querystring[queryindex] > 126 || queryindex >= MAXQUERYSTRLEN) ? 1 : 0;
					if (finished == 0 && querystring[queryindex] == '=')
					{
						// Jump over '='
						while (querystring[queryindex] == '=' && queryindex < MAXQUERYSTRLEN)
						{
							queryindex++;
						}
						int valueindex = 0;
						while ( querystring[queryindex] >= 32 && querystring[queryindex] < 127 && querystring[queryindex] != '='
                                && querystring[queryindex] != '&' && valueindex < MAXQUERYVALLEN && queryindex < MAXQUERYSTRLEN)
						{
							valstring[valueindex] = querystring[queryindex];
							queryindex++;
							valueindex++;
						}

						if (valueindex >= MAXQUERYVALLEN)
						{
							IPSCAN_LOG( LOGPREFIX "ipscan: query parameter value string is too long : %s\n", querystring);
							valueindex = MAXQUERYVALLEN; // Truncate
						}
						valstring[valueindex]=0; // Add termination

						rc = sscanf(valstring,"%"SCNd64, &varval );
						if (rc == 1)
						{
							// Mark the entry as valid, increment the number of queries found
							query[numqueries].varval = varval;
							query[numqueries].valid = 1;
							#ifdef QUERYDEBUG
							IPSCAN_LOG( LOGPREFIX "ipscan: Added a new query name: %s with a value of : %"PRId64"\n", query[numqueries].varname, query[numqueries].varval);
							#endif
							numqueries++;
						}
						else
						{
							#ifdef QUERYDEBUG
							IPSCAN_LOG( LOGPREFIX "ipscan: Bad value assignment for %s, setting invalid.\n", query[numqueries].varname);
							#endif
							query[numqueries].varval = 0;
							query[numqueries].valid = 0;
							numqueries++;
						}
					}
					// Move past the '&' sign
					while (querystring[queryindex] == '&' && queryindex < MAXQUERYSTRLEN && finished == 0)
					{
						queryindex++;
					}
					finished = (querystring[queryindex] < 32 || queryindex >= MAXQUERYSTRLEN) ? 1 : 0;
				}
				#ifdef QUERYDEBUG
				IPSCAN_LOG( LOGPREFIX "ipscan: Number of query pairs found is : %d\n", numqueries);
				#endif
			}
		}
		else if (strncmp("HEAD", requestmethod, 4) == 0)
		{
			// Create the header
			create_html_common_header();
			// Now finish the header
        		printf("<TITLE>IPv6 Port Scanner Version %s</TITLE>\n", IPSCAN_VER);
        		printf("</HEAD>\n");
		        printf("</HTML>\n");
			IPSCAN_LOG( LOGPREFIX "ipscan: HEAD request method, sending headers only\n");
			exit(EXIT_SUCCESS);
		}
		else
		{
			IPSCAN_LOG( LOGPREFIX "ipscan: WARNING: called with an unsupported request method: %s.\n", requestmethod);
			// Create the header
			create_html_common_header();
			// Now finish the header
			printf("<TITLE>IPv6 Port Scanner Version %s</TITLE>\n", IPSCAN_VER);
			printf("</HEAD>\n");
			printf("<BODY>\n");
			printf("<P>I was called with an unsupported request-method. That is very disappointing.</P>\n");
			// Finish the html
			create_html_body_end();
			exit(EXIT_FAILURE);
		}
	}

	// Determine the clients' address
	remoteaddrvar = getenv("REMOTE_ADDR");
	if(NULL == remoteaddrvar)
	{
		IPSCAN_LOG( LOGPREFIX "ipscan: ERROR: REMOTE_ADDR variable lookup returned NULL.\n");
	}
	else if (strnlen(remoteaddrvar,(INET6_ADDRSTRLEN+1)) > INET6_ADDRSTRLEN)
	{
		IPSCAN_LOG( LOGPREFIX "ipscan: ERROR: REMOTE_ADDR variable length exceeds allocated buffer size (%d > %d)\n", (int)strnlen(remoteaddrvar, (INET6_ADDRSTRLEN+1)), INET6_ADDRSTRLEN);
		exit(EXIT_FAILURE);
	}
	else if( sscanf(remoteaddrvar,"%"TO_STR(INET6_ADDRSTRLEN)"s",remoteaddrstring) != 1 )
	{
		IPSCAN_LOG( LOGPREFIX "ipscan: Invalid REMOTE_ADDR variable data.\n");
	}
	else
	{	
		// Determine the remote host address
		rc = inet_pton(AF_INET6, remoteaddrstring, remotehost);
		if (rc <= 0)
		{
			IPSCAN_LOG( LOGPREFIX "ipscan: ERROR: Unparseable IPv6 host address : %s\n", remoteaddrstring);
			exit(EXIT_FAILURE);
		}
		else
		{
			remotehost_msb = 0ULL;
			remotehost_lsb = 0ULL;

			// Split address into two 64 bit values stored within database
			for (i=0 ; i<8 ; i++)
			{
				shift = 8 * (7-i);
				value = (remotehost[i]);
				while (shift > 0)
				{
					value = (value << 8);
					shift -= 8;
				}
				remotehost_msb |= value;
				shift = 8 * (7-i);
				value = (remotehost[8+i]);
				while (shift > 0)
				{
					value = (value << 8);
					shift -= 8;
				}
				remotehost_lsb |= value;
			}

			#ifdef DEBUG
			IPSCAN_LOG( LOGPREFIX "ipscan: Remote host address %"PRIx64":%"PRIx64"\n", remotehost_msb, remotehost_lsb);
			#endif

		}
	}


	// If query string is empty then we generate the introductory html/form for the client

	if (numqueries == 0)
	{
		// Create the header
		create_html_common_header();
		// Create the main html body
		create_html_form(DEFNUMPORTS, NUMUDPPORTS, &portlist[0], &udpportlist[0]);
		// Finish the html
		create_html_body_end();
	}

	// Following is a query, so determine the passed parameters and decide whether we
	// need to initiate a scan, return the current result set or a summary of scans

	else
	{

		// includeexisting should only be passed the values -1 or 1, set to 0 if not present
		// or an unsuitable value is passed.

		i = 0;
		while (i < numqueries && strncmp("includeexisting",query[i].varname,15)!= 0) i++;
		if (i < numqueries && query[i].valid == 1)
		{
			if ( abs((int) query[i].varval) == 1 )
			{
				includeexisting = query[i].varval;
			}
			else
			{
				includeexisting = 0 ;
			}
		}
		else
		{
			includeexisting = 0;
		}

		// Begin the reconstitution of the query string
		rc = snprintf(reconptr, reconquerysize, "includeexisting=%d", (int)includeexisting);
		if (rc > 16 && rc < 19)
		{
			reconptr += rc;
			reconquerysize -= rc;
			if (reconquerysize <= 0)
			{
				IPSCAN_LOG( LOGPREFIX "ipscan: ERROR: run out of room to reconstitute query, please increase MAXQUERYSTRLEN (%d) and recompile.\n", MAXQUERYSTRLEN);
				exit(EXIT_FAILURE);
			}
		}
		else
		{
			IPSCAN_LOG( LOGPREFIX "ipscan: ERROR: attempt to reconstitute query returned an unexpected length (%d, expecting 17 or 18)\n", rc);
			exit(EXIT_FAILURE);
		}

		// Determine whether existing ports are to be included in the tested list or not:
		if (includeexisting == 1)
		{
			// custom ports will be appended to the default ports list
			numports = DEFNUMPORTS;
		}
		else
		{
			// default ports will be overwritten by any custom ports
			numports = 0;
		}

		#ifdef QUERYDEBUG
		IPSCAN_LOG( LOGPREFIX "ipscan: numports is initially found to be %d\n", numports);
		#endif

		//
		// Add in the custom ports if they're valid and NOT already present in the portlist ...
		//

		int customport = 0;
		char cpnum[16];
		int cplen;

		// Counter holding the number of received customportN statements
		unsigned int numcustomports = 0;

		while (customport < NUMUSERDEFPORTS)
		{
			cplen = snprintf(cpnum, 16, "customport%d", customport);
			i = 0;
			while (i < numqueries && strncmp(cpnum,query[i].varname,cplen)!= 0) i++;

			// If customportN parameter exists then increment the counter, irrespective of whether
			// the parameter was valid or not
			if (i < numqueries) numcustomports++;

			// If the parameter is valid then perform further checks
			if (i < numqueries && query[i].valid == 1)
			{
				// Check the port number is in the valid range
				if (query[i].varval >=MINVALIDPORT && query[i].varval <= MAXVALIDPORT)
				{
					j = 0;
					while (j < numports && portlist[j].port_num != query[i].varval) j++;
					// if this customport is not one of the ports already destined for checking then
					// add it to the port list
					if (j == numports)
					{
						portlist[numports].port_num = query[i].varval;
						portlist[numports].special = 0;
						rc = snprintf(&portlist[numports].port_desc[0], PORTDESCSIZE, "User-specified: %d",(int)query[i].varval);
						if (rc < 0 || rc >= PORTDESCSIZE)
						{
							IPSCAN_LOG( LOGPREFIX "ipscan: WARNING: failed to write user-specified port description, does PORTDESCSIZE (%d) need increasing?\n", PORTDESCSIZE);
						}
						numports ++;
						rc = snprintf(reconptr, reconquerysize, "&customport%d=%d", customport, (int)query[i].varval);
						// &customport (11); cpnum (1-5) ; = (1) ; portnum (1-5)
						if (rc >= 14 && rc <= 22)
						{
							reconptr += rc;
							reconquerysize -= rc;
							if (reconquerysize <= 0)
							{
								IPSCAN_LOG( LOGPREFIX "ipscan: ERROR: run out of room to reconstitute query, please increase MAXQUERYSTRLEN (%d) and recompile.\n", MAXQUERYSTRLEN);
								exit(EXIT_FAILURE);
							}
						}
						else
						{
							IPSCAN_LOG( LOGPREFIX "ipscan: ERROR: customport%d reconstitution failed, due to unexpected size.\n", customport);
							exit(EXIT_FAILURE);
						}
					}
				}
			}
			customport++;
		}

		// Look for magic query string
		// NEVER use the summary facility on an internet-facing host.
		i = 0;
		magic = -1;
		while (i < numqueries && strncmp("magic",query[i].varname,5)!= 0) i++;
		if (i < numqueries && query[i].valid == 1)
		{
			magic = query[i].varval;
		}

		// Look for the starttime query string, set it to -1 if not present or invalid
		i = 0;
		querystarttime = -1;
		while (i < numqueries && strncmp("starttime",query[i].varname,9)!= 0) i++;
		if (i < numqueries && query[i].valid == 1)
		{
			if (query[i].varval >= 0)
			{
				querystarttime = query[i].varval;
			}
		}

		// Look for the session query string, set it to -1 if not present or invalid
		i = 0;
		querysession = -1;
		while (i < numqueries && strncmp("session",query[i].varname,7)!= 0) i++;
		if (i < numqueries && query[i].valid == 1)
		{
			if (query[i].varval >= 0)
			{
				querysession = query[i].varval;
			}
		}

		// Look for the beginscan query string, return 0 if not present or incorrect value
		i = 0;
		beginscan = 0;
		while (i < numqueries && strncmp("beginscan",query[i].varname,9)!= 0) i++;
		if (i < numqueries && query[i].valid == 1)
		{
			beginscan = (query[i].varval == MAGICBEGIN ) ? 1 : 0;
		}

		// Look for the fetch query string
		i = 0;
		fetch = 0;
		while (i < numqueries && strncmp("fetch",query[i].varname,5)!= 0) i++;
		if (i < numqueries && query[i].valid == 1)
		{
			fetch = (query[i].varval >0) ? 1 : 0;
			#if (TEXTMODE != 1)
			if (1 == fetch) fetchnum = query[i].varval;
			#endif
		}

		// Dump the variables resulting from the query-string parsing
		#ifdef QUERYDEBUG
			IPSCAN_LOG( LOGPREFIX "ipscan: DEBUG info: numqueries = %d\n", numqueries);
			#if (TEXTMODE != 1)
			IPSCAN_LOG( LOGPREFIX "ipscan: DEBUG info: includeexisting = %d beginscan = %d fetch = %d fetchnum = %d\n", includeexisting, beginscan, fetch, fetchnum);
			#else
			IPSCAN_LOG( LOGPREFIX "ipscan: DEBUG info: includeexisting = %d beginscan = %d fetch = %d\n", includeexisting, beginscan, fetch);
			#endif
			IPSCAN_LOG( LOGPREFIX "ipscan: DEBUG info: session = %"PRIu64" starttime = %"PRIu64" and numports = %d\n", \
					session, (uint64_t)starttime, numports);
			IPSCAN_LOG( LOGPREFIX "ipscan: DEBUG info: querysession = %"PRId64" querystarttime = %"PRId64" magic = %"PRId64"\n", querysession, querystarttime, magic );
			IPSCAN_LOG( LOGPREFIX "ipscan: DEBUG info: numcustomports = %d NUMUSERDEFPORTS = %d\n", numcustomports, NUMUSERDEFPORTS );
			IPSCAN_LOG( LOGPREFIX "ipscan: DEBUG info: reconstituted query string = %s\n", reconquery );
		#endif

		//
		//
		//
		// NOW DETERMINE WHAT TO DO ......
		//
		//
		//


		//
		// NON-Javascript mode of operation (text browser compatible)
		//
		#if (TEXTMODE == 1)
		// *IF* we have everything we need to initiate the scan/results page then we
		// should have been passed (1+NUMUSERDEFPORTS) queries
		// i.e. includeexisting (either +1 or -1) and customports 0 thru n params

		if ( numqueries >= (NUMUSERDEFPORTS + 1) && (numcustomports == NUMUSERDEFPORTS) && includeexisting != 0 )
		{

			#if (IPSCAN_LOGVERBOSITY == 1)
			time_t scanstart = starttime;
			#endif

			// Create the header
			create_html_common_header();
			// Create main output
			printf("<TITLE>IPv6 Port Scanner Version %s</TITLE>\n", IPSCAN_VER);
			printf("</HEAD>\n");
			printf("<BODY>\n");
			printf("<H3 style=\"color:red\">IPv6 Port Scanner by Tim Chappell</H3>\n");
			printf("<P>Results for host : %s</P>\n\n", remoteaddrstring);

			printf("<P>Scan beginning at: %s, expected to take up to %d seconds ...</P>\n", \
					asctime(localtime(&starttime)), (int)ESTIMATEDTIMETORUN );

			// Only included if ping is compiled in ...
			#if (IPSCAN_INCLUDE_PING == 1)
			// Ping the remote host and store the result ...
			pingresult = check_icmpv6_echoresponse(remoteaddrstring, starttime, session, indirecthost);
			result = (pingresult >= IPSCAN_INDIRECT_RESPONSE) ? (pingresult - IPSCAN_INDIRECT_RESPONSE) : pingresult ;

			#if (IPSCAN_LOGVERBOSITY == 1)
			IPSCAN_LOG( LOGPREFIX "ipscan: INFO: ICMPv6 ping of %s returned %d (%s), from host %s\n",remoteaddrstring, pingresult, resultsstruct[result].label, indirecthost);
			#endif

			portsstats[result]++ ;

			rc = write_db(remotehost_msb, remotehost_lsb, starttime, session, (0 + (IPSCAN_PROTO_ICMPV6 << IPSCAN_PROTO_SHIFT)), pingresult, indirecthost);
			if (rc != 0)
			{
				IPSCAN_LOG( LOGPREFIX "ipscan: ERROR : write_db for ping result returned : %d\n", rc);
			}
			#endif

			#if (IPSCAN_LOGVERBOSITY == 1)
			IPSCAN_LOG( LOGPREFIX "ipscan: Beginning scan of %d UDP ports on client : %s\n", numudpports, remoteaddrstring);
			#endif

			#if (IPSCAN_INCLUDE_PING == 1)
			printf("<P>ICMPv6 ECHO-Request:</P>\n");
			printf("<TABLE border=\"1\">\n");
			printf("<TR style=\"text-align:left\">\n");
			if (pingresult >= IPSCAN_INDIRECT_RESPONSE)
			{
				printf("<TD TITLE=\"IPv6 ping\">ICMPv6 ECHO REQUEST returned : </TD><TD style=\"background-color:%s\">INDIRECT-%s (from %s)</TD>\n",resultsstruct[result].colour,resultsstruct[result].label, indirecthost);
			}
			else
			{
				printf("<TD TITLE=\"IPv6 ping\">ICMPv6 ECHO REQUEST returned : </TD><TD style=\"background-color:%s\">%s</TD>\n",resultsstruct[result].colour,resultsstruct[result].label);
			}
			printf("</TR>\n");
			printf("</TABLE>\n");
			#endif

			// Scan the UDP ports in parallel
			remaining = numudpports;
			porti = 0;
			numchildren = 0;
			rc = 0;
			while (remaining > 0 || numchildren > 0)
			{
				while (remaining > 0)
				{
					if (numchildren < MAXUDPCHILDREN && remaining > 0)
					{
						int todo = (remaining > MAXUDPPORTSPERCHILD) ? MAXUDPPORTSPERCHILD : remaining;
						#ifdef UDPPARLLDEBUG
						IPSCAN_LOG( LOGPREFIX "ipscan: INFO: check_udp_ports_parll(%s,%d,%d,host_msb,host_lsb,starttime,session,portlist)\n",remoteaddrstring,porti,todo);
						#endif
						rc |= check_udp_ports_parll(remoteaddrstring, porti, todo, remotehost_msb, remotehost_lsb, starttime, session, &udpportlist[0]);
						porti += todo;
						numchildren ++;
						remaining = (numudpports - porti);
					}
					if (numchildren == MAXUDPCHILDREN && remaining > 0)
					{
						int pid = wait(&childstatus);
						numchildren--;
						if (childstatus != 0) IPSCAN_LOG( LOGPREFIX "ipscan: WARNING: UDP ongoing phase : PID=%d retired with status=%d, numchildren is now %d\n", pid, childstatus, numchildren );
					}
				}
				while (numchildren > 0)
				{
					int pid = wait(&childstatus);
					numchildren--;
					if (childstatus != 0) IPSCAN_LOG( LOGPREFIX "ipscan: WARNING: UDP shutdown phase : PID=%d retired with status=%d, numchildren is now %d\n", pid, childstatus, numchildren );
				}
			}

			if (rc != 0)
			{
				IPSCAN_LOG( LOGPREFIX "ipscan: INFO: check_udp_ports_parll() exited with ORed value of %d\n",rc);
			}

			#if (IPSCAN_INCLUDE_UDP == 1)
			printf("<P>Individual UDP port scan results:</P>\n");
			// Start of UDP port scan results table
			printf("<TABLE border=\"1\">\n");
			for (portindex= 0; portindex < NUMUDPPORTS ; portindex++)
			{
				port = udpportlist[portindex].port_num;
				special = udpportlist[portindex].special;
				last = (portindex == (NUMUDPPORTS-1)) ? 1 : 0 ;
				result = read_db_result(remotehost_msb, remotehost_lsb, starttime, session, (port + ((special & IPSCAN_SPECIAL_MASK) << IPSCAN_SPECIAL_SHIFT) + (IPSCAN_PROTO_UDP << IPSCAN_PROTO_SHIFT) ));

				#ifdef UDPDEBUG
				if (0 != special)
				{
					IPSCAN_LOG( LOGPREFIX "ipscan: INFO: UDP port %d:%d returned %d(%s)\n", port, special, result, resultsstruct[result].label);
				}
				else
				{
					IPSCAN_LOG( LOGPREFIX "ipscan: INFO: UDP port %d returned %d(%s)\n", port, result, resultsstruct[result].label);
				}
				#endif

				// Start of a new row, so insert the appropriate tag if required
				if (position ==0) printf("<TR>");

				// Find a matching returnval, or else flag it as unknown
				i = 0 ;
				while (i < NUMRESULTTYPES && resultsstruct[i].returnval != result) i++;
				if (result == resultsstruct[i].returnval)
				{
					portsstats[result]++ ;
					if (0 != special)
					{
						printf("<TD TITLE=\"%s\" style=\"background-color:%s\">Port %d[%d] = %s</TD>", udpportlist[portindex].port_desc, resultsstruct[i].colour, port, special, resultsstruct[i].label);
					}
					else
					{
						printf("<TD TITLE=\"%s\" style=\"background-color:%s\">Port %d = %s</TD>", udpportlist[portindex].port_desc, resultsstruct[i].colour, port, resultsstruct[i].label);
					}
				}
				else
				{
					if (0 != special)
					{
						printf("<TD TITLE=\"%s\" style=\"background-color:white\">Port %d[%d] = BAD</TD>", udpportlist[portindex].port_desc, port, special);
						IPSCAN_LOG( LOGPREFIX "ipscan: WARNING: Unknown result for UDP port %d:%d is %d\n", port, special, result);
					}
					else
					{
						printf("<TD TITLE=\"%s\" style=\"background-color:white\">Port %d = BAD</TD>", udpportlist[portindex].port_desc, port);
						IPSCAN_LOG( LOGPREFIX "ipscan: WARNING: Unknown result for UDP port %d is %d\n", port, result);
					}
					portsstats[ PORTUNKNOWN ]++ ;
				}

				// Get ready for the next cell, add the end of row tag if required
				position++;
				if (position >= TXTMAXCOLS || last == 1) { printf("</TR>\n"); position=0; };

			}
			printf("</TABLE>\n");
			#endif

			#if (IPSCAN_LOGVERBOSITY == 1)
			IPSCAN_LOG( LOGPREFIX "ipscan: Beginning scan of %d TCP ports on client : %s\n", numports, remoteaddrstring);
			#endif
			printf("<P>Individual TCP port scan results:</P>\n");

			// Scan the TCP ports in parallel
			remaining = numports;
			porti = 0;
			numchildren = 0;
			rc = 0;
			while (remaining > 0 || numchildren > 0)
			{
				while (remaining > 0)
				{
					if (numchildren < MAXCHILDREN && remaining > 0)
					{
						int todo = (remaining > MAXPORTSPERCHILD) ? MAXPORTSPERCHILD : remaining;
						#ifdef PARLLDEBUG
						IPSCAN_LOG( LOGPREFIX "ipscan: INFO: check_tcp_ports_parll(%s,%d,%d,host_msb,host_lsb,starttime,session,portlist)\n",remoteaddrstring,porti,todo);
						#endif
						rc |= check_tcp_ports_parll(remoteaddrstring, porti, todo, remotehost_msb, remotehost_lsb, starttime, session, &portlist[0]);
						porti += todo;
						numchildren ++;
						remaining = (numports - porti);
					}
					if (numchildren == MAXCHILDREN && remaining > 0)
					{
						int pid = wait(&childstatus);
						numchildren--;
						if (childstatus != 0) IPSCAN_LOG( LOGPREFIX "ipscan: WARNING: ongoing phase : PID=%d retired with status=%d, numchildren is now %d\n", pid, childstatus, numchildren );
					}
				}
				while (numchildren > 0)
				{
					int pid = wait(&childstatus);
					numchildren--;
					if (childstatus != 0) IPSCAN_LOG( LOGPREFIX "ipscan: WARNING: shutdown phase : PID=%d retired with status=%d, numchildren is now %d\n", pid, childstatus, numchildren );
				}
			}

			if (rc != 0)
			{
				IPSCAN_LOG( LOGPREFIX "ipscan: INFO: check_tcp_ports_parll() exited with ORed value of %d\n",rc);
			}

			// Start of TCP port scan results table
			printf("<TABLE border=\"1\">\n");
			for (portindex= 0; portindex < numports ; portindex++)
			{
				port = portlist[portindex].port_num;
				special = portlist[portindex].special;
				last = (portindex == (numports-1)) ? 1 : 0 ;
				result = read_db_result(remotehost_msb, remotehost_lsb, starttime, session, (port + ((special & IPSCAN_SPECIAL_MASK) << IPSCAN_SPECIAL_SHIFT)+ (IPSCAN_PROTO_TCP << IPSCAN_PROTO_SHIFT)) );

				#ifdef RESULTSDEBUG
				if (0 != special)
				{
					IPSCAN_LOG( LOGPREFIX "ipscan: INFO: TCP port %d:%d returned %d(%s)\n", port, special, result, resultsstruct[result].label);
				}
				else
				{
					IPSCAN_LOG( LOGPREFIX "ipscan: INFO: TCP port %d returned %d(%s)\n", port, result, resultsstruct[result].label);
				}
				#endif

				// Start of a new row, so insert the appropriate tag if required
				if (position ==0) printf("<TR>");

				// Find a matching returnval, or else flag it as unknown
				i = 0 ;
				while (i < NUMRESULTTYPES && resultsstruct[i].returnval != result) i++;
				if (result == resultsstruct[i].returnval)
				{
					portsstats[result]++ ;
					if (0 != special)
					{
						printf("<TD TITLE=\"%s\" style=\"background-color:%s\">Port %d[%d] = %s</TD>", portlist[portindex].port_desc, resultsstruct[i].colour, port, special, resultsstruct[i].label);
					}
					else
					{
						printf("<TD TITLE=\"%s\" style=\"background-color:%s\">Port %d = %s</TD>", portlist[portindex].port_desc, resultsstruct[i].colour, port, resultsstruct[i].label);
					}

				}
				else
				{
					if (0 != special)
					{
						printf("<TD TITLE=\"%s\" style=\"background-color:white\">Port %d[%d] = BAD</TD>", portlist[portindex].port_desc, port, special);
						IPSCAN_LOG( LOGPREFIX "ipscan: WARNING: Unknown result for TCP port %d:%d is %d\n", port, special, result);
					}
					else
					{
						printf("<TD TITLE=\"%s\" style=\"background-color:white\">Port %d = BAD</TD>", portlist[portindex].port_desc, port);
						IPSCAN_LOG( LOGPREFIX "ipscan: WARNING: Unknown result for TCP port %d is %d\n",port,result);
					}
					portsstats[ PORTUNKNOWN ]++ ;
				}

				// Get ready for the next cell, add the end of row tag if required
				position++;
				if (position >= TXTMAXCOLS || last == 1) { printf("</TR>\n"); position=0; };

			}
			printf("</TABLE>\n");

			time_t nowtime = time(0);
			if (nowtime < 0)
			{
				IPSCAN_LOG( LOGPREFIX "ipscan: ERROR: time() returned bad value for nowtime %d (%s)\n", errno, strerror(errno));
			}
			printf("<P>Scan of %d ports complete at: %s.</P>\n", numports, asctime(localtime(&nowtime)));

			// Create results key table
			create_results_key_table(remoteaddrstring, starttime);
			// Finish the output
			create_html_body_end();

			#if (IPSCAN_LOGVERBOSITY == 1)
			time_t scancomplete = time(0);
			if (scancomplete < 0)
			{
				IPSCAN_LOG( LOGPREFIX "ipscan: ERROR: time() returned bad value for scancomplete %d (%s)\n", errno, strerror(errno));
			}
			IPSCAN_LOG( LOGPREFIX "ipscan: INFO: port scan and html document generation took %d seconds\n", (int)(scancomplete - scanstart));
			#endif

			// Log the summary of results internally
			i = 0;
			position = 0;
			while (i < NUMRESULTTYPES)
			{
				if (position == 0)
				{
					rc = snprintf(logbufferptr, logbuffersize, "Found %d %s",portsstats[i], resultsstruct[i].label );
				}
				else
				{
					rc = snprintf(logbufferptr, logbuffersize, ", %d %s", portsstats[i], resultsstruct[i].label);
				}

				if (rc < 0 || rc >= logbuffersize)
				{
					IPSCAN_LOG( LOGPREFIX "ipscan: ERROR: logbuffer write truncated, increase LOGENTRYLEN (currently %d) and recompile.\n", LOGENTRYLEN);
					exit(EXIT_FAILURE);
				}

				logbufferptr += rc ;
				logbuffersize -= rc;
				position ++ ;
				if ( position >= LOGMAXCOLS || i == (NUMRESULTTYPES -1) )
				{
					#if (IPSCAN_LOGVERBOSITY == 1)
					IPSCAN_LOG( LOGPREFIX "ipscan: %s\n", logbuffer);
					#endif
					logbufferptr = &logbuffer[0];
					logbuffersize = LOGENTRYLEN;
					position = 0;
				}
				i++ ;
			}

			// Delete the results now that we're done
			rc = delete_from_db(remotehost_msb, remotehost_lsb, starttime, session);
			if (rc != 0)
			{
				IPSCAN_LOG( LOGPREFIX "ipscan: ERROR: delete_from_db return code was %d (expected 0)\n", rc);
				exit(EXIT_FAILURE);
			}
		}
		#else

		// These handle the calls by the javascript version of the tester

		// *IF* we have everything we need to query the database ...
		// querysession, querystarttime, fetch and includeexisting. Could also have one or more customports
		// javascript updateurl always minimally includes includeexisting

		if ( numqueries >= 4 && querysession >= 0 && querystarttime >= 0 && beginscan == 0 && fetch == 1 && includeexisting != 0 && IPSCAN_SUCCESSFUL_COMPLETION <= fetchnum)
		{
			// Put out a dummy page to keep the webserver happy
			create_html_common_header();
			printf("<TITLE>IPv6 Port Scanner Version %s</TITLE>\n", IPSCAN_VER);
			printf("</HEAD>\n");
			printf("<BODY>\n");
			printf("<P>End of test - dummy response.</P>\n");
			// Finish the output
			create_html_body_end();

			#if (IPSCAN_LOGVERBOSITY == 1)
			if (IPSCAN_SUCCESSFUL_COMPLETION == fetchnum)
			{
				IPSCAN_LOG( LOGPREFIX "ipscan: fetch indicated SUCCESSFUL completion.\n");
			}
			else if (IPSCAN_HTTPTIMEOUT_COMPLETION == fetchnum)
			{
				IPSCAN_LOG( LOGPREFIX "ipscan: fetch indicated HTTP TIMEOUT completion.\n");
			}
			else if (IPSCAN_UNSUCCESSFUL_COMPLETION == fetchnum)
			{
				IPSCAN_LOG( LOGPREFIX "ipscan: fetch indicated UNSUCCESSFUL completion.\n");
			}
			else if (IPSCAN_EVAL_ERROR == fetchnum)
			{
				IPSCAN_LOG( LOGPREFIX "ipscan: fetch indicated EVAL ERROR.\n");
			}
			else if (IPSCAN_OTHER_ERROR == fetchnum)
			{
				IPSCAN_LOG( LOGPREFIX "ipscan: fetch indicated OTHER ERROR.\n");
			}
			else if (IPSCAN_NAVIGATE_AWAY == fetchnum)
			{
				IPSCAN_LOG( LOGPREFIX "ipscan: fetch indicated user NAVIGATED AWAY.\n");
			}
			else if (IPSCAN_UNEXPECTED_CHANGE <= fetchnum)
			{
				IPSCAN_LOG( LOGPREFIX "ipscan: fetch indicated UNEXPECTED CHANGE: %d\n", fetchnum);
			}
			#endif

			if (IPSCAN_SUCCESSFUL_COMPLETION == fetchnum || IPSCAN_UNSUCCESSFUL_COMPLETION == fetchnum)
			{
				// Store an indication in the database that the test results have all been received
				// by the browser and so the results can be safely deleted ...
				rc = write_db(remotehost_msb, remotehost_lsb, (uint64_t)querystarttime, (uint64_t)querysession, (0 + (IPSCAN_PROTO_TESTSTATE << IPSCAN_PROTO_SHIFT)), IPSCAN_TESTSTATE_COMPLETE, unusedfield);
				if (rc != 0)
				{
					IPSCAN_LOG( LOGPREFIX "ipscan: ERROR: write_db for IPSCAN_TESTSTATE COMPLETE returned non-zero: %d\n", rc);
				}
			}
		}
		else if ( numqueries >= 4 && querysession >= 0 && querystarttime >= 0 && beginscan == 0 && fetch == 1 && includeexisting != 0  && IPSCAN_SUCCESSFUL_COMPLETION > fetchnum)
		{
			// Simplified header in which to wrap array of results
			create_json_header();
			// Dump the current port results for this client, starttime and session
			rc = dump_db(remotehost_msb, remotehost_lsb, (uint64_t)querystarttime, (uint64_t)querysession);
			if (rc != 0)
			{
				IPSCAN_LOG( LOGPREFIX "ipscan: ERROR: dump_db return code was %d (expected 0)\n", rc);
				exit(EXIT_FAILURE);
			}
		}


		// *IF* we have everything we need to initiate the scan
		// session, starttime, beginscan, includeexisting and >=0 userdefined ports [NOTE: no fetch]

		else if ( numqueries >= 4 && querysession >= 0 && querystarttime >= 0 && beginscan == 1 && includeexisting != 0 && fetch == 0)
		{
			time_t scanstart = time(0);
			if (scanstart < 0)
			{
				IPSCAN_LOG( LOGPREFIX "ipscan: ERROR: time() returned bad value for scanstart %d (%s)\n", errno, strerror(errno));
			}

			// Put out a dummy page to keep the webserver happy
			// Creating this page will take the entire duration of the scan ...
			create_html_common_header();
			printf("<TITLE>IPv6 Port Scanner Version %s</TITLE>\n", IPSCAN_VER);
			printf("</HEAD>\n");
			printf("<BODY>\n");

			// Generate database entry for test state
			rc = write_db(remotehost_msb, remotehost_lsb, (uint64_t)querystarttime, (uint64_t)querysession, (0 + (IPSCAN_PROTO_TESTSTATE << IPSCAN_PROTO_SHIFT)), IPSCAN_TESTSTATE_RUNNING, unusedfield);
			if (rc != 0)
			{
				IPSCAN_LOG( LOGPREFIX "ipscan: ERROR: write_db for IPSCAN_PROTO_TESTSTATE RUNNING returned non-zero: %d\n", rc);
			}

			// Only include this section if ping is compiled in ...
			#if (IPSCAN_INCLUDE_PING == 1)
			pingresult = check_icmpv6_echoresponse(remoteaddrstring, (uint64_t)querystarttime, (uint64_t)querysession, indirecthost);
			result = (pingresult >= IPSCAN_INDIRECT_RESPONSE) ? (pingresult - IPSCAN_INDIRECT_RESPONSE) : pingresult ;
			#if (IPSCAN_LOGVERBOSITY == 1)
			IPSCAN_LOG( LOGPREFIX "ipscan: INFO: ICMPv6 ping of %s returned %d (%s), from host %s\n",remoteaddrstring, pingresult, resultsstruct[result].label, indirecthost);
			#endif
			portsstats[result]++ ;
			rc = write_db(remotehost_msb, remotehost_lsb, (uint64_t)querystarttime, (uint64_t)querysession, (0 + (IPSCAN_PROTO_ICMPV6 << IPSCAN_PROTO_SHIFT)), pingresult, indirecthost);
			if (rc != 0)
			{
				IPSCAN_LOG( LOGPREFIX "ipscan: ERROR: write_db for ping result returned non-zero: %d\n", rc);
				create_html_body_end();
				exit(EXIT_FAILURE);
			}
			#endif

			#if (IPSCAN_LOGVERBOSITY == 1)
			IPSCAN_LOG( LOGPREFIX "ipscan: Beginning scan of %d UDP ports on client : %s\n", numudpports, remoteaddrstring);
			#endif

			// Only included if UDP is compiled in ...
			#if (IPSCAN_INCLUDE_UDP == 1)
			// Scan the UDP ports in parallel
			remaining = numudpports;
			porti = 0;
			numchildren = 0;
			while (remaining > 0 || numchildren > 0)
			{
				while (remaining > 0)
				{
					if (numchildren < MAXUDPCHILDREN && remaining > 0)
					{
						int todo = (remaining > MAXUDPPORTSPERCHILD) ? MAXUDPPORTSPERCHILD : remaining;
						#ifdef UDPPARLLDEBUG
						IPSCAN_LOG( LOGPREFIX "ipscan: INFO: check_udp_ports_parll(%s,%d,%d,host_msb,host_lsb,starttime,session,portlist)\n",remoteaddrstring,porti,todo);
						#endif
						rc = check_udp_ports_parll(remoteaddrstring, porti, todo, remotehost_msb, remotehost_lsb, (uint64_t)querystarttime, (uint64_t)querysession, &udpportlist[0]);
						porti += todo;
						numchildren ++;
						remaining = (numudpports - porti);
					}
					if (numchildren == MAXUDPCHILDREN && remaining > 0)
					{
						int pid = wait(&childstatus);
						numchildren--;
						if (childstatus != 0) IPSCAN_LOG( LOGPREFIX "ipscan: WARNING: UDP ongoing phase : PID=%d retired with status=%d, numchildren is now %d\n", pid, childstatus, numchildren );
					}
				}
				while (numchildren > 0)
				{
					int pid = wait(&childstatus);
					numchildren--;
					if (childstatus != 0) IPSCAN_LOG( LOGPREFIX "ipscan: WARNING: UDP shutdown phase : PID=%d retired with status=%d, numchildren is now %d\n", pid, childstatus, numchildren );
				}
			}
			#endif

			#if (IPSCAN_LOGVERBOSITY == 1)
			IPSCAN_LOG( LOGPREFIX "ipscan: Beginning scan of %d TCP ports on client : %s\n", numports, remoteaddrstring);
			#endif

			// Scan the TCP ports in parallel
			remaining = numports;
			porti = 0;
			numchildren = 0;
			while (remaining > 0 || numchildren > 0)
			{
				while (remaining > 0)
				{
					if (numchildren < MAXCHILDREN && remaining > 0)
					{
						int todo = (remaining > MAXPORTSPERCHILD) ? MAXPORTSPERCHILD : remaining;
						#ifdef PARLLDEBUG
						IPSCAN_LOG( LOGPREFIX "ipscan: INFO: check_tcp_ports_parll(%s,%d,%d,host_msb,host_lsb,starttime,session,portlist)\n",remoteaddrstring,porti,todo);
						#endif
						rc = check_tcp_ports_parll(remoteaddrstring, porti, todo, remotehost_msb, remotehost_lsb, (uint64_t)querystarttime, (uint64_t)querysession, &portlist[0]);
						porti += todo;
						numchildren ++;
						remaining = (numports - porti);
					}
					if (numchildren == MAXCHILDREN && remaining > 0)
					{
						int pid = wait(&childstatus);
						numchildren--;
						if (childstatus != 0) IPSCAN_LOG( LOGPREFIX "ipscan: WARNING: ongoing phase : PID=%d retired with status=%d, numchildren is now %d\n", pid, childstatus, numchildren );
					}
				}
				while (numchildren > 0)
				{
					int pid = wait(&childstatus);
					numchildren--;
					if (childstatus != 0) IPSCAN_LOG( LOGPREFIX "ipscan: WARNING: shutdown phase : PID=%d retired with status=%d, numchildren is now %d\n", pid, childstatus, numchildren );
				}
			}

			// Only included if UDP is compiled in ...
			#if (IPSCAN_INCLUDE_UDP == 1)
			// Generate the stats
			for (portindex= 0; portindex < NUMUDPPORTS ; portindex++)
			{
				port = udpportlist[portindex].port_num;
				special = udpportlist[portindex].special;
				result = read_db_result(remotehost_msb, remotehost_lsb, (uint64_t)querystarttime, (uint64_t)querysession, (port + ((special & IPSCAN_SPECIAL_MASK) << IPSCAN_SPECIAL_SHIFT) + (IPSCAN_PROTO_UDP << IPSCAN_PROTO_SHIFT) ) );

				// Find a matching returnval, or else flag it as unknown
				i = 0 ;
				while (i < NUMRESULTTYPES && resultsstruct[i].returnval != result) i++;
				if (result == resultsstruct[i].returnval)
				{
					portsstats[result]++ ;
				}
				else
				{
					if (0 != special)
					{
						IPSCAN_LOG( LOGPREFIX "ipscan: WARNING scan of UDP port %d:%d returned : %d\n", port, special, result);
					}
					else
					{
						IPSCAN_LOG( LOGPREFIX "ipscan: WARNING scan of UDP port %d returned : %d\n", port, result);
					}
					portsstats[PORTUNKNOWN]++;
				}
			}
			#endif

			for (portindex= 0; portindex < numports ; portindex++)
			{
				port = portlist[portindex].port_num;
				special = portlist[portindex].special;
				result = read_db_result(remotehost_msb, remotehost_lsb, (uint64_t)querystarttime, (uint64_t)querysession, (port + ((special & IPSCAN_SPECIAL_MASK) << IPSCAN_SPECIAL_SHIFT) + (IPSCAN_PROTO_TCP << IPSCAN_PROTO_SHIFT) ));

				// Find a matching returnval, or else flag it as unknown
				i = 0 ;
				while (i < NUMRESULTTYPES && resultsstruct[i].returnval != result) i++;
				if (result == resultsstruct[i].returnval)
				{
					portsstats[result]++ ;
				}
				else
				{
					if (0 != special)
					{
						IPSCAN_LOG( LOGPREFIX "ipscan: WARNING scan of TCP port %d:%d returned : %d\n", port, special, result);
					}
					else
					{
						IPSCAN_LOG( LOGPREFIX "ipscan: WARNING scan of TCP port %d returned : %d\n", port, result);
					}
					portsstats[PORTUNKNOWN]++;
				}
			}

			#ifdef DEBUG
			IPSCAN_LOG( LOGPREFIX "ipscan: rmthost        was : %"PRIx64":%"PRIx64"\n", remotehost_msb, remotehost_lsb);
			IPSCAN_LOG( LOGPREFIX "ipscan: querystarttime was : %"PRId64"\n", querystarttime);
			IPSCAN_LOG( LOGPREFIX "ipscan: querysession   was : %"PRId64"\n", querysession);
			IPSCAN_LOG( LOGPREFIX "ipscan: numcustomports was : %d\n", numcustomports);
			#endif


			// Finish the output
			create_html_body_end();

			#if (IPSCAN_LOGVERBOSITY == 1)
			time_t scancomplete = time(0);
			if (scancomplete < 0)
			{
				IPSCAN_LOG( LOGPREFIX "ipscan: ERROR: time() returned bad value for scancomplete %d (%s)\n", errno, strerror(errno));
			}
			IPSCAN_LOG( LOGPREFIX "ipscan: INFO: port scan and html document generation took %d seconds\n", (int)(scancomplete - scanstart));
			#endif

			// Log the summary of results internally
			i = 0;
			position = 0;
			while (i < NUMRESULTTYPES)
			{
				if (position == 0)
				{
					rc = snprintf(logbufferptr, logbuffersize, "Found %d %s",portsstats[i], resultsstruct[i].label );
				}
				else
				{
					rc = snprintf(logbufferptr, logbuffersize, ", %d %s", portsstats[i], resultsstruct[i].label);
				}

				if (rc < 0 || rc >= logbuffersize)
				{
					IPSCAN_LOG( LOGPREFIX "ipscan: ERROR: logbuffer write truncated, increase LOGENTRYLEN (currently %d) and recompile.\n", LOGENTRYLEN);
					exit(EXIT_FAILURE);
				}

				logbufferptr += rc ;
				logbuffersize -= rc;
				position ++ ;
				if ( position >= LOGMAXCOLS || i == (NUMRESULTTYPES -1) )
				{
					#if (IPSCAN_LOGVERBOSITY == 1)
					IPSCAN_LOG( LOGPREFIX "ipscan: %s\n", logbuffer);
					#endif
					logbufferptr = &logbuffer[0];
					logbuffersize = LOGENTRYLEN;
					position = 0;
				}
				i++ ;
			}

			// Wait until the javascript client has flagged the test as complete or we've run out of time ...
			time_t deletenowtime = time(0);
			if (deletenowtime < 0)
			{
				IPSCAN_LOG( LOGPREFIX "ipscan: ERROR: time() returned bad value for deletenowtime %d (%s)\n", errno, strerror(errno));
			}
			time_t timeouttime = (scanstart + IPSCAN_DELETE_TIMEOUT);
			unsigned int client_finished = 0;
			while (deletenowtime < timeouttime && client_finished == 0)
			{
				result = read_db_result(remotehost_msb, remotehost_lsb, (uint64_t)querystarttime, (uint64_t)querysession, (0 + (IPSCAN_PROTO_TESTSTATE << IPSCAN_PROTO_SHIFT) ) );
				if (IPSCAN_TESTSTATE_COMPLETE == result) client_finished = 1;
				sleep(IPSCAN_TESTSTATE_COMPLETE_SLEEP);
				deletenowtime = time(0);
				if (deletenowtime < 0)
				{
					IPSCAN_LOG( LOGPREFIX "ipscan: ERROR: time() returned bad value for deletenowtime %d (%s)\n", errno, strerror(errno));
				}
			}

			#if (IPSCAN_LOGVERBOSITY == 1)
			if (client_finished == 1)
			{
				IPSCAN_LOG( LOGPREFIX "ipscan: Exited test-complete loop because client signalled.\n");
			}
			else
			{
				IPSCAN_LOG( LOGPREFIX "ipscan: Exited test-complete loop with no client response.\n");
			}
			#endif

			#ifdef DBDEBUG
			IPSCAN_LOG( LOGPREFIX "ipscan: Deleting test results for %"PRIx64"%"PRIx64"\n", remotehost_msb, remotehost_lsb);
			IPSCAN_LOG( LOGPREFIX "ipscan: starttime   was : %"PRId64"\n", scanstart);
			IPSCAN_LOG( LOGPREFIX "ipscan: timeouttime was : %"PRId64"\n", timeouttime);
			#endif

			// Delete the results
			rc = delete_from_db(remotehost_msb, remotehost_lsb, (uint64_t)querystarttime, (uint64_t)querysession);
			if (rc != 0)
			{
				IPSCAN_LOG( LOGPREFIX "ipscan: ERROR: delete_from_db return code was %d (expected 0)\n", rc);
			}

		}

		// *IF* we have everything we need to create the standard HTML page
		// we should have been passed (1+NUMUSERDEFPORTS) queries
		// i.e. includeexisting (either +1 or -1) and customports 0 thru n params

		else if (numqueries >= (NUMUSERDEFPORTS + 1) && numcustomports == NUMUSERDEFPORTS && includeexisting != 0 && beginscan == 0 && fetch == 0)
		{
			#if (IPSCAN_LOGVERBOSITY == 1)
			IPSCAN_LOG( LOGPREFIX "ipscan: Creating the standard web results page start point\n");
			#endif

			// Create the header
			create_html_header(session, starttime, numports, numudpports, reconquery);
			// Create the main html body
			create_html_body(remoteaddrstring, starttime, numports, numudpports, &portlist[0], &udpportlist[0]);
			// Finish the html
			create_html_body_end();
		}

		#endif

		//
		// Cases common to both modes of operation
		//

		#if (SUMMARYENABLE == 1)
		// Generate a summary of scans - limited to IPv6 addresses and time/date
		else if (numqueries == 1 && magic == MAGICSUMMARY )
		{
			create_html_common_header();
			printf("<TITLE>IPv6 Port Scanner Version %s</TITLE>\n", IPSCAN_VER);
			printf("</HEAD>\n");
			printf("<BODY>\n");
			printf("<H3 style=\"color:red\">IPv6 Port Scanner by Tim Chappell</H3>\n");
			printf("<P>Summary of Scans:</P>\n");
			// Output the scan summary
			rc = summarise_db();
			// Finish the output
			create_html_body_end();
			IPSCAN_LOG( LOGPREFIX "ipscan: summarise_db: provided the requested summary, exited with rc=%d\n", rc);
		}
		#endif

		else
		{
			// Dummy report - most likely to be triggered via a hackers attempt to pass unusual query parameters
			create_html_common_header();
			printf("<TITLE>IPv6 Port Scanner Version %s</TITLE>\n", IPSCAN_VER);
			printf("</HEAD>\n");
			printf("<BODY>\n");
			printf("<P>Nothing useful to report.</P>\n");
			#if (IPSCAN_BAD_URL_HELP != 0)
			printf("<P>You seem to have presented an incomplete or unexpected query string to ipscan. ");
			printf("If you are trying to automate ipscan operation then please see the following ");
			printf("<A href=\"%s\">Scan Automation link.</A></P>\n", IPSCAN_BAD_URL_LINK);
			#endif
			// Finish the output
			create_html_body_end();
			IPSCAN_LOG( LOGPREFIX "ipscan: Something untoward happened, numqueries = %d, magic = %"PRId64"\n", numqueries, magic);
			IPSCAN_LOG( LOGPREFIX "ipscan: includeexisting = %d, beginscan = %d, fetch = %d,\n", includeexisting, beginscan, fetch);
			IPSCAN_LOG( LOGPREFIX "ipscan: querysession = %"PRId64" querystarttime = %"PRId64" numports = %d and numcustomports = %d.\n", \
					querysession, querystarttime, numports, numcustomports);
		}
	}
	exit(EXIT_SUCCESS);
}

///////////////////////////////////////////////////////////////////////
// prompro.cpp -- Main program for Prompro-8 EPROM Programmer
// Date: Sun Feb  8 16:34:07 2015  (C) Warren W. Gay VE3WWG 
///////////////////////////////////////////////////////////////////////

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <fcntl.h>
#include <termios.h>
#include <poll.h>

#include "pugixml.hpp"

#include <string>
#include <map>
#include <vector>

static bool cmd_debug = false;			// When true, show serial trafic for debugging
static bool xml_loaded = false;
static bool verbose = false;			// Verbose messages when true
static unsigned baud_rate = 0;
static bool rtscts = false;
static std::string device;
static struct termios term;
static int rtimeout_ms = 2000;			// Read timeout in ms
static std::string eprom_type;			// EPROM type
static std::string prompro_type;		// Current prompro-8 EPROM type
static std::string download;			// Download file name

static int serial = -1;

struct s_segment {
	std::string	ppname;			// Prompro name
	unsigned	offset;			// Byte offset
};

struct s_eprom_type {
	std::string		name;		// Config name for this EPROM
	unsigned		segsize;	// Segment size
	std::vector<s_segment>	segs;		// Segment description
};

static s_eprom_type	*eprom = 0;		// Currently selected EPROM type

static std::map<std::string,s_eprom_type> eproms;

//////////////////////////////////////////////////////////////////////
// Wait for any key
//////////////////////////////////////////////////////////////////////

static void
anykey() {
	char ch;
	int rc;

	do	{
		rc = read(0,&ch,1);
	} while ( rc == -1 && errno == EINTR );
}

//////////////////////////////////////////////////////////////////////
// Poll for input:
// Returns:
//	1	At least one byte available to be read
//	0	Timeout occurred
//	-1	Error occurred (dev no longer open?)
//////////////////////////////////////////////////////////////////////

static int
pollch(int timeout_ms) {
        struct pollfd pinfo;
	int rc;

        pinfo.fd = serial;
        pinfo.events = POLLIN;
        pinfo.revents = 0;

	rc = poll(&pinfo,1,timeout_ms);

	if ( rc < 1 && cmd_debug ) {
		fprintf(stderr,"poll(timeout=%d ms) returned %d",timeout_ms,rc);
		if ( rc < 0 )
			fprintf(stderr," (%s)\n",strerror(errno));
		else	fputc('\n',stderr);
		fflush(stderr);
	}

	return rc;
}

//////////////////////////////////////////////////////////////////////
// Read 1 byte else timeout (-1 is return upon timeout)
//////////////////////////////////////////////////////////////////////

static int
readch(int timeout) {
	int rc = pollch(timeout);
	unsigned char ch;

	if ( rc == -1 ) {
		fprintf(stderr,"ERROR %s: reading device %s\n",
			strerror(errno),
			device.c_str());
		exit(3);
	}

	if ( rc == 0 )
		return -1;		// Indicate timeout

	do	{
		rc = read(serial,&ch,1);
	} while ( rc == -1 && errno == EINTR );

	assert(rc == 1);

	if ( cmd_debug ) {
		if ( isprint(ch) ) {
			fprintf(stderr," <= '%c'\n",ch);
		} else	{
			fprintf(stderr," <= 0x%02X\n",ch);
		}
	}

	return int(ch);
}

static void
writech(const char *data) {
	int n = strlen(data);
	int rc;

	do	{
		rc = write(serial,data,n);
	} while ( rc == -1 && errno == EINTR );
	
	if ( cmd_debug && n > 0 ) {
		for ( int x=0; x<n; ++x ) {
			char ch = data[x];

			if ( isprint(ch) ) {
				fprintf(stderr," => '%c'\n",ch);
			} else	{
				fprintf(stderr," => 0x%02X\n",ch);
			}
		}
	}

	assert(rc == rc);
}

static void
writecr() {
	writech("\r");
}

static void
timeout(const char *message) {
	fputs("TIMEOUT: ",stderr);
	fputs(message,stderr);
	fputs("\n",stderr);
	exit(13);
}

static bool
get_prompt(int timeout_ms=0) {
	char ch;

	if ( timeout_ms <= 0 )
		timeout_ms = rtimeout_ms;

	do	{
		ch = readch(timeout_ms);
		if ( ch == -1 )
			return false;	// Timeout
	} while ( ch != '*' );
	
	return true;
}

static void
select_type(const char *type) {

	writech("S");
	writech(type);
	writecr();
	if ( !get_prompt(6000) )
		timeout("Selecting PROMPRO EPROM type");
}

static void
select_type(const s_segment& seg) {

	if ( prompro_type != seg.ppname ) {
		if ( verbose )
			printf("Selecting PROMPRO type %s\n",seg.ppname.c_str());
		select_type(seg.ppname.c_str());
		prompro_type = seg.ppname;
	} else	{
		if ( verbose )
			printf("Continuing to use PROMPRO type %s\n",seg.ppname.c_str());
	}
}

static void
select_type() {
	
	if ( eprom->segs.size() < 1 ) {
		fprintf(stderr,"XML misconfiguration for EPROM type '%s'\n",
			eprom->name.c_str());
		exit(1);
	}

	s_segment& seg = eprom->segs[0];
	select_type(seg);
}


static void
download_file(const char *path) {
	FILE *dfile = fopen(path,"w");

	if ( !dfile ) {
		fprintf(stderr,"%s: Opening file %s for write.\n",
			strerror(errno),
			path);
		exit(2);
	}

	if ( verbose )
		printf("Downloading EPROM to file '%s'\n",
			download.c_str());

	for ( auto it = eprom->segs.begin(); it != eprom->segs.end(); ++it ) {
		const s_segment& seg = *it;

		select_type(seg);
	}

	fclose(dfile);
}

static void
load_xml(const char *pathname) {
	pugi::xml_document doc;
	pugi::xml_parse_result res;

	res = doc.load_file(pathname);
	if ( !res ) {
		fprintf(stderr,"ERROR %s: at file offset %ld of file %s\n",
			res.description(),
			res.offset,
			pathname);
		return;
	}

	pugi::xml_node prompro_node = doc.child("prompro");

	{
		pugi::xml_node serial_node = prompro_node.child("serial");
		pugi::xml_attribute baud_attr = serial_node.attribute("baud");
		pugi::xml_attribute device_attr = serial_node.attribute("device");
		pugi::xml_attribute rtscts_attr = serial_node.attribute("rtscts");

		if ( !baud_attr.empty() )
			baud_rate = baud_attr.as_uint();
		if ( !device_attr.empty() )
			device = device_attr.value();
		if ( !rtscts_attr.empty() )
			rtscts = !!rtscts_attr.as_int();
	}

	{
		pugi::xml_node eproms_node = prompro_node.child("eproms");
		
		for ( auto it=eproms_node.begin(); it != eproms_node.end(); ++it ) {
			pugi::xml_node eprom_node = *it;
			s_eprom_type etype;

			etype.name = eprom_node.attribute("type").value();
			etype.segsize = eprom_node.attribute("segsize").as_uint();

			for ( auto i2=eprom_node.begin(); i2 != eprom_node.end(); ++ i2 ) {
				pugi::xml_node seg_node = *i2;
				s_segment eseg;

				eseg.ppname = seg_node.attribute("use").value();
				eseg.offset = seg_node.attribute("offset").as_uint();
				etype.segs.push_back(eseg);
			}

			eproms[etype.name] = etype;
		}
	}

	{
		pugi::xml_node dflts_node = prompro_node.child("defaults");
		pugi::xml_attribute eprom_type_attr = dflts_node.attribute("eprom");

		if ( !eprom_type_attr.empty() )
			eprom_type = eprom_type_attr.value();
	}

	xml_loaded = true;
}

static void
usage() {
	
	fputs(	"Usage: prompro [-d file] [-e eprom_type] [-h]\n"
		"where:\n"
		"\t-d file\t\tDownload EPROM to file\n"
		"\t-e eprom_type\tSpecify configured eprom type\n"
		"\t-v\t\tVerbose messages\n"
		"\t-D\t\tEnable debugging output\n"
		"\t-h\t\tThis info\n",
		stdout);
	exit(0);
}

int
main(int argc,char **argv) {
	std::string xml_path = getenv("HOME");
	int optch;

	//////////////////////////////////////////////////////////////
	// Load from XML config file(s) for defaults
	//////////////////////////////////////////////////////////////

	xml_path += "/.prompro.xml";

	if ( !access(xml_path.c_str(),F_OK) )
		load_xml(xml_path.c_str());

	if ( !access(".prompro.xml",F_OK) )
		load_xml("./.prompro.xml");

	if ( !xml_loaded ) {
		fprintf(stderr,"Missing or invalid ~/.prompro.xml and/or ./.prompro.xml files.\n");
		exit(1);
	}

	if ( cmd_debug )
		printf("Dev='%s', baud=%u, rtscts=%d, eprom=%s\n",
			device.c_str(),
			baud_rate,
			rtscts,
			eprom_type.c_str());

	//////////////////////////////////////////////////////////////
	// Process command line arguments
	//////////////////////////////////////////////////////////////

	while ( (optch = getopt(argc, argv, ":hd:e:Dv")) != -1 ) {
		switch ( optch ) {
		case 'd':			// Download EPROM
			download = optarg;
			break;
		case 'e':
			eprom_type = optarg;
			break;
		case 'D':
			cmd_debug = true;
			break;
		case 'v':
			verbose = true;
			break;
		case 'h':
			usage();
			break;
		case '?':
			printf("Unknown option -%c\n", optopt);
			exit(1);
		default:
			printf("Invalid argument '%c'\n",optch);
			exit(1);
		}
	}

	//////////////////////////////////////////////////////////////
	// Check that the eprom type is known
	//////////////////////////////////////////////////////////////

	{
		auto it = eproms.find(eprom_type);
		if ( it == eproms.end() ) {
			fprintf(stderr,"Unknown EPROM type '%s'\n",eprom_type.c_str());
			exit(1);
		}

		eprom = &it->second;		
		if ( verbose )
			printf("EPROM Type: %s\n",eprom->name.c_str());
	}

	//////////////////////////////////////////////////////////////
	// Open the serial device
	//////////////////////////////////////////////////////////////

	serial = open(device.c_str(),O_RDWR,0);
	if ( serial == -1 ) {
		fprintf(stderr,"%s: Unable to open serial device %s\n",
			strerror(errno),
			device.c_str());
		exit(2);
	}

	if ( tcgetattr(serial,&term) < 0 ) {
		fprintf(stderr,"%s: getting serial port attributes of %s\n",
			strerror(errno),
			device.c_str());
		exit(2);
	}

	tcflush(serial,TCIOFLUSH);		// Flush all in/out chars in transit
	cfmakeraw(&term);			// Setup for raw I/O
	cfsetspeed(&term,baud_rate);		// Set baud rate
	term.c_cflag |= PARODD | PARENB;	// Set odd parity
	if ( rtscts ) {
		term.c_cflag |= CRTSCTS;	// Enable RTS/CTS flow control
	} else	{
		term.c_cflag &= ~CRTSCTS;	// Disable RTS/CTS flow control
	}

	if ( tcsetattr(serial,TCSANOW,&term) < 0 ) { // Apply changes to serial port
		fprintf(stderr,"%s: Setting serial port attributes of %s\n",
			strerror(errno),
			device.c_str());
	}

	writech("\r");

	if ( !get_prompt() ) {
		fputs("PROMPRO-8 is not ready.\n",stderr);
		exit(4);
	}

	//////////////////////////////////////////////////////////////
	// Select EPROM type
	//////////////////////////////////////////////////////////////

	select_type();

	puts("Place EPROM in socket, and press CR when ready:");
	anykey();

	//////////////////////////////////////////////////////////////
	// Check for downloads
	//////////////////////////////////////////////////////////////

	if ( download != "" )
		download_file(download.c_str());

	close(serial);

	return 0;
}

// End prompro.cpp

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
#include <assert.h>
#include <fcntl.h>
#include <termios.h>
#include <poll.h>

#include "pugixml.hpp"

#include <string>
#include <map>
#include <vector>

static bool xml_loaded = false;
static unsigned baud_rate = 0;
static bool rtscts = false;
static std::string device;
static struct termios term;
static int rtimeout_ms = 2000;			// Read timeout in ms
static std::string	eprom_type;		// EPROM type

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

static std::map<std::string,s_eprom_type> eproms;

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

        pinfo.fd = serial;
        pinfo.events = POLLIN;
        pinfo.revents = 0;

	return poll(&pinfo,1,timeout_ms);
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

	return int(ch);
}

static void
writech(const char *data) {
	int n = strlen(data);
	int rc;

	do	{
		rc = write(serial,data,n);
	} while ( rc == -1 && errno == EINTR );
	
	assert(rc == rc);
}

static bool
get_prompt(int timeout_ms=0) {
	char ch;

	if ( timeout_ms <= 0 )
		timeout_ms = rtimeout_ms;

	do	{
		ch = readch(rtimeout_ms);
		if ( ch == -1 )
			return false;	// Timeout
	} while ( ch != '*' );
	
	return true;
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

int
main(int argc,char **argv) {
	std::string xml_path = getenv("HOME");

	xml_path += "/.prompro.xml";

	if ( !access(xml_path.c_str(),F_OK) )
		load_xml(xml_path.c_str());

	if ( !access(".prompro.xml",F_OK) )
		load_xml("./.prompro.xml");

	if ( !xml_loaded ) {
		fprintf(stderr,"Missing or invalid ~/.prompro.xml and/or ./.prompro.xml files.\n");
		exit(1);
	}

	printf("Dev='%s', baud=%u, rtscts=%d, eprom=%s\n",
		device.c_str(),
		baud_rate,
		rtscts,
		eprom_type.c_str());

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

	puts("Ready.");

	close(serial);

	return 0;
}

// End prompro.cpp

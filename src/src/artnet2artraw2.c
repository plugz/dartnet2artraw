#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/time.h>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <getopt.h>
#include <assert.h>

#include <fcntl.h>
#include <ctype.h>

#include <limits.h>

#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>

#include "pcap.h"
#include "osdep/osdep.h"
#include "crypto.h"
#include "common.h"
#include "ieee80211.h"

#define RTC_RESOLUTION  8192

#define REQUESTS    30
#define MAX_APS     20

#define NEW_IV  1
#define RETRY   2
#define ABORT   3

#define DEAUTH_REQ      \
    "\xC0\x00\x3A\x01\xCC\xCC\xCC\xCC\xCC\xCC\xBB\xBB\xBB\xBB\xBB\xBB" \
    "\xBB\xBB\xBB\xBB\xBB\xBB\x00\x00\x07\x00"

#define AUTH_REQ        \
    "\xB0\x00\x3A\x01\xBB\xBB\xBB\xBB\xBB\xBB\xCC\xCC\xCC\xCC\xCC\xCC" \
    "\xBB\xBB\xBB\xBB\xBB\xBB\xB0\x00\x00\x00\x01\x00\x00\x00"

#define ASSOC_REQ       \
    "\x00\x00\x3A\x01\xBB\xBB\xBB\xBB\xBB\xBB\xCC\xCC\xCC\xCC\xCC\xCC"  \
    "\xBB\xBB\xBB\xBB\xBB\xBB\xC0\x00\x31\x04\x64\x00"

#define REASSOC_REQ       \
    "\x20\x00\x3A\x01\xBB\xBB\xBB\xBB\xBB\xBB\xCC\xCC\xCC\xCC\xCC\xCC"  \
    "\xBB\xBB\xBB\xBB\xBB\xBB\xC0\x00\x31\x04\x64\x00\x00\x00\x00\x00\x00\x00"

#define NULL_DATA       \
    "\x48\x01\x3A\x01\xBB\xBB\xBB\xBB\xBB\xBB\xCC\xCC\xCC\xCC\xCC\xCC"  \
    "\xBB\xBB\xBB\xBB\xBB\xBB\xE0\x1B"

#define RTS             \
    "\xB4\x00\x4E\x04\xBB\xBB\xBB\xBB\xBB\xBB\xCC\xCC\xCC\xCC\xCC\xCC"

#define RATES           \
    "\x01\x04\x02\x04\x0B\x16\x32\x08\x0C\x12\x18\x24\x30\x48\x60\x6C"

#define PROBE_REQ       \
    "\x40\x00\x00\x00\xFF\xFF\xFF\xFF\xFF\xFF\xCC\xCC\xCC\xCC\xCC\xCC"  \
    "\xFF\xFF\xFF\xFF\xFF\xFF\x00\x00"

char usage[] =

"\n"
"  artnet2artraw2 - (C) 2006-2013 Thomas d\'Otreppe\n"
"  http://www.aircrack-ng.org\n"
"\n"
"  usage: aireplay-ng <options> <replay interface>\n"
"\n"
"      --help              : Displays this usage screen\n"
"\n";


struct options
{
    int f_minlen;
    int f_maxlen;
    int f_tods;
    int f_fromds;

    char ip_out[16];    //16 for 15 chars + \x00
    int port_out;
    char *iface_out;
}
opt;

struct devices
{
    int fd_in,  arptype_in;
    int fd_out, arptype_out;

    unsigned char mac_in[6];
    unsigned char mac_out[6];

    int is_wlanng;
    int is_hostap;
    int is_madwifi;
    int is_madwifing;
    int is_bcm43xx;

    FILE *f_cap_in;

    struct pcap_file_header pfh_in;
}
dev;

static struct wif *_wi_in, *_wi_out;

struct APt
{
    unsigned char set;
    unsigned char found;
    unsigned char len;
    unsigned char essid[255];
    unsigned char bssid[6];
    unsigned char chan;
    unsigned int  ping[REQUESTS];
    int  pwr[REQUESTS];
};

struct APt ap[MAX_APS];

unsigned long nb_pkt_sent;
unsigned char h80211[4096];
unsigned char tmpbuf[4096];
char strbuf[512];

unsigned char ska_auth1[]     = "\xb0\x00\x3a\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
                        "\x00\x00\x00\x00\x00\x00\xb0\x01\x01\x00\x01\x00\x00\x00";

unsigned char ska_auth3[4096] = "\xb0\x40\x3a\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
                        "\x00\x00\x00\x00\x00\x00\xc0\x01";


int send_packet(void *buf, size_t count)
{
	struct wif *wi = _wi_out; /* XXX globals suck */
	unsigned char *pkt = (unsigned char*) buf;

	if( (count > 24) && (pkt[1] & 0x04) == 0 && (pkt[22] & 0x0F) == 0)
	{
		pkt[22] = (nb_pkt_sent & 0x0000000F) << 4;
		pkt[23] = (nb_pkt_sent & 0x00000FF0) >> 4;
	}

	if (wi_write(wi, buf, count, NULL) == -1) {
		switch (errno) {
		case EAGAIN:
		case ENOBUFS:
			usleep(10000);
			return 0; /* XXX not sure I like this... -sorbo */
		}

		perror("wi_write()");
		return -1;
	}

	nb_pkt_sent++;
	return 0;
}

int read_packet(void *buf, size_t count, struct rx_info *ri)
{
	struct wif *wi = _wi_in; /* XXX */
	int rc;

        rc = wi_read(wi, buf, count, ri);
        if (rc == -1) {
            switch (errno) {
            case EAGAIN:
                    return 0;
            }

            perror("wi_read()");
            return -1;
        }

	return rc;
}

/**
    if bssid != NULL its looking for a beacon frame
*/
int grab_essid(unsigned char* packet, int len)
{
    int i=0, j=0, pos=0, tagtype=0, taglen=0, chan=0;
    unsigned char bssid[6];

    memcpy(bssid, packet+16, 6);
    taglen = 22;    //initial value to get the fixed tags parsing started
    taglen+= 12;    //skip fixed tags in frames
    do
    {
        pos    += taglen + 2;
        tagtype = packet[pos];
        taglen  = packet[pos+1];
    } while(tagtype != 3 && pos < len-2);

    if(tagtype != 3) return -1;
    if(taglen != 1) return -1;
    if(pos+2+taglen > len) return -1;

    chan = packet[pos+2];

    pos=0;

    taglen = 22;    //initial value to get the fixed tags parsing started
    taglen+= 12;    //skip fixed tags in frames
    do
    {
        pos    += taglen + 2;
        tagtype = packet[pos];
        taglen  = packet[pos+1];
    } while(tagtype != 0 && pos < len-2);

    if(tagtype != 0) return -1;
    if(taglen > 250) taglen = 250;
    if(pos+2+taglen > len) return -1;

    for(i=0; i<20; i++)
    {
        if( ap[i].set)
        {
            if( memcmp(bssid, ap[i].bssid, 6) == 0 )    //got it already
            {
                if(packet[0] == 0x50 && !ap[i].found)
                {
                    ap[i].found++;
                }
                if(ap[i].chan == 0) ap[i].chan=chan;
                break;
            }
        }
        if(ap[i].set == 0)
        {
            for(j=0; j<taglen; j++)
            {
                if(packet[pos+2+j] < 32 || packet[pos+2+j] > 127)
                {
                    return -1;
                }
            }

            ap[i].set = 1;
            ap[i].len = taglen;
            memcpy(ap[i].essid, packet+pos+2, taglen);
            ap[i].essid[taglen] = '\0';
            memcpy(ap[i].bssid, bssid, 6);
            ap[i].chan = chan;
            if(packet[0] == 0x50) ap[i].found++;
            return 0;
        }
    }
    return -1;
}

static int get_ip_port(char *iface, char *ip, const int ip_size)
{
	char *host;
	char *ptr;
	int port = -1;
	struct in_addr addr;

	host = strdup(iface);
	if (!host)
		return -1;

	ptr = strchr(host, ':');
	if (!ptr)
		goto out;

	*ptr++ = 0;

	if (!inet_aton(host, (struct in_addr *)&addr))
		goto out; /* XXX resolve hostname */

	if(strlen(host) > 15)
        {
            port = -1;
            goto out;
        }
	strncpy(ip, host, ip_size);
	port = atoi(ptr);
        if(port <= 0) port = -1;

out:
	free(host);
	return port;
}

struct net_hdr {
	uint8_t		nh_type;
	uint32_t	nh_len;
	uint8_t		nh_data[0];
};

int tcp_test(const char* ip_str, const short port)
{
    int sock, i;
    struct sockaddr_in s_in;
    int packetsize = 1024;
    unsigned char packet[packetsize];
    struct timeval tv, tv2, tv3;
    int caplen = 0;
    int times[REQUESTS];
    int min, avg, max, len;
    struct net_hdr nh;

    tv3.tv_sec=0;
    tv3.tv_usec=1;

    s_in.sin_family = PF_INET;
    s_in.sin_port = htons(port);
    if (!inet_aton(ip_str, &s_in.sin_addr))
            return -1;

    if ((sock = socket(s_in.sin_family, SOCK_STREAM, IPPROTO_TCP)) == -1)
            return -1;

    /* avoid blocking on reading the socket */
    if( fcntl( sock, F_SETFL, O_NONBLOCK ) < 0 )
    {
        perror( "fcntl(O_NONBLOCK) failed" );
        return( 1 );
    }

    gettimeofday( &tv, NULL );

    while (1)  //waiting for relayed packet
    {
        if (connect(sock, (struct sockaddr*) &s_in, sizeof(s_in)) == -1)
        {
            if(errno != EINPROGRESS && errno != EALREADY)
            {
                perror("connect");
                close(sock);

                printf("Failed to connect\n");

                return -1;
            }
        }
        else
        {
            gettimeofday( &tv2, NULL );
            break;
        }

        gettimeofday( &tv2, NULL );
        //wait 3000ms for a successful connect
        if (((tv2.tv_sec*1000000 - tv.tv_sec*1000000) + (tv2.tv_usec - tv.tv_usec)) > (3000*1000))
        {
            printf("Connection timed out\n");
            close(sock);
            return(-1);
        }
        usleep(10);
    }

    PCT; printf("TCP connection successful\n");

    //trying to identify airserv-ng
    memset(&nh, 0, sizeof(nh));
//     command: GET_CHAN
    nh.nh_type	= 2;
    nh.nh_len	= htonl(0);

    if (send(sock, &nh, sizeof(nh), 0) != sizeof(nh))
    {
        perror("send");
        return -1;
    }

    gettimeofday( &tv, NULL );
    i=0;

    while (1)  //waiting for GET_CHAN answer
    {
        caplen = read(sock, &nh, sizeof(nh));

        if(caplen == -1)
        {
            if( errno != EAGAIN )
            {
                perror("read");
                return -1;
            }
        }

        if(caplen > 0 && (unsigned)caplen == sizeof(nh))
        {
            len = ntohl(nh.nh_len);
            if (len <= packetsize && len > 0)
            {
                if( nh.nh_type == 1 && i==0 )
                {
                    i=1;
                    caplen = read(sock, packet, len);
                    if(caplen == len)
                    {
                        i=2;
                        break;
                    }
                    else
                    {
                        i=0;
                    }
                }
                else
                {
                    caplen = read(sock, packet, len);
                }
            }
        }

        gettimeofday( &tv2, NULL );
        //wait 1000ms for an answer
        if (((tv2.tv_sec*1000000 - tv.tv_sec*1000000) + (tv2.tv_usec - tv.tv_usec)) > (1000*1000))
        {
            break;
        }
        if(caplen == -1)
            usleep(10);
    }

    if(i==2)
    {
        PCT; printf("airserv-ng found\n");
    }
    else
    {
        PCT; printf("airserv-ng NOT found\n");
    }

    close(sock);

    for(i=0; i<REQUESTS; i++)
    {
        if ((sock = socket(s_in.sin_family, SOCK_STREAM, IPPROTO_TCP)) == -1)
                return -1;

        /* avoid blocking on reading the socket */
        if( fcntl( sock, F_SETFL, O_NONBLOCK ) < 0 )
        {
            perror( "fcntl(O_NONBLOCK) failed" );
            return( 1 );
        }

        usleep(1000);

        gettimeofday( &tv, NULL );

        while (1)  //waiting for relayed packet
        {
            if (connect(sock, (struct sockaddr*) &s_in, sizeof(s_in)) == -1)
            {
                if(errno != EINPROGRESS && errno != EALREADY)
                {
                    perror("connect");
                    close(sock);

                    printf("Failed to connect\n");

                    return -1;
                }
            }
            else
            {
                gettimeofday( &tv2, NULL );
                break;
            }

            gettimeofday( &tv2, NULL );
            //wait 1000ms for a successful connect
            if (((tv2.tv_sec*1000000 - tv.tv_sec*1000000) + (tv2.tv_usec - tv.tv_usec)) > (1000*1000))
            {
                break;
            }
            //simple "high-precision" usleep
            select(1, NULL, NULL, NULL, &tv3);
        }
        times[i] = ((tv2.tv_sec*1000000 - tv.tv_sec*1000000) + (tv2.tv_usec - tv.tv_usec));
        printf( "\r%d/%d\r", i, REQUESTS);
        fflush(stdout);
        close(sock);
    }

    min = INT_MAX;
    avg = 0;
    max = 0;

    for(i=0; i<REQUESTS; i++)
    {
        if(times[i] < min) min = times[i];
        if(times[i] > max) max = times[i];
        avg += times[i];
    }
    avg /= REQUESTS;

    PCT; printf("ping %s:%d (min/avg/max): %.3fms/%.3fms/%.3fms\n", ip_str, port, min/1000.0, avg/1000.0, max/1000.0);

    return 0;
}

int do_attack_test()
{
    unsigned char packet[4096];
    struct timeval tv, tv2, tv3;
    int len=0, i=0, j=0, k=0;
    int gotit=0, answers=0, found=0;
    int caplen=0, essidlen=0;
    unsigned int min, avg, max;
    int ret=0;
    float avg2;
    struct rx_info ri;
    int atime=200;  //time in ms to wait for answer packet (needs to be higher for airserv)
    unsigned char nulldata[1024];

    if(opt.port_out > 0)
    {
        atime += 200;
        PCT; printf("Testing connection to injection device %s\n", opt.iface_out);
        ret = tcp_test(opt.ip_out, opt.port_out);
        if(ret != 0)
        {
            return( 1 );
        }
        printf("\n");

        /* open the replay interface */
        _wi_out = wi_open(opt.iface_out);
        if (!_wi_out)
            return 1;
        printf("\n");
        dev.fd_out = wi_fd(_wi_out);
        wi_get_mac(_wi_out, dev.mac_out);
        if(1)
        {
            _wi_in = _wi_out;
            dev.fd_in = dev.fd_out;

            /* XXX */
            dev.arptype_in = dev.arptype_out;
            wi_get_mac(_wi_in, dev.mac_in);
        }
    }

    if(1)
    {
        /* avoid blocking on reading the socket */
        if( fcntl( dev.fd_in, F_SETFL, O_NONBLOCK ) < 0 )
        {
            perror( "fcntl(O_NONBLOCK) failed" );
            return( 1 );
        }
    }

    srand( time( NULL ) );

    memset(ap, '\0', 20*sizeof(struct APt));

    PCT; printf("Trying broadcast probe requests...\n");

    memcpy(h80211, PROBE_REQ, 24);

    len = 24;

    h80211[24] = 0x00;      //ESSID Tag Number
    h80211[25] = 0x00;      //ESSID Tag Length

    len += 2;

    //memcpy(h80211+len, RATES, 16);

    //len += 16;
    //memcpy(h80211+len, RATES, 16);

    //len += 16;
    //memcpy(h80211+len, RATES, 16);

    //len += 16;

    gotit=0;
    answers=0;

    //memcpy(h80211, NULL_DATA, sizeof(NULL_DATA) - 1);
    //len = sizeof(NULL_DATA) - 1;

    // Avec TYPE_DATA, on peut recevoir 4 octets. (a partir de data[24])
    //h80211[0] = IEEE80211_FC0_TYPE_DATA | IEEE80211_FC0_SUBTYPE_CFPOLL;

    //h80211[0] = IEEE80211_FC0_TYPE_MGT | IEEE80211_FC0_SUBTYPE_ASSOC_REQ;
    //h80211[0] = IEEE80211_FC0_TYPE_MGT | IEEE80211_FC0_SUBTYPE_ASSOC_RESP;
    //h80211[0] = IEEE80211_FC0_TYPE_MGT | IEEE80211_FC0_SUBTYPE_REASSOC_REQ;

    char* str =
        "0123456789"
        "0123456789"
        "0123456789"
        "0123456789"
        "0123456789"
        "0123456789"
        "0123456789"
        "0123456789"
        "0123456789"
        "0123456789"
        "0123456789"
        "0123456789"
        "0123456789"
        "0123456789"
        "0123456789"
        "0123456789"
        "0123456789"
        "0123456789"
        "0123456789";

    memcpy(h80211+4, str, 6*3); // MAC ADDRESS * 3

    h80211[len++] = IEEE80211_ELEMID_CHALLENGE;
    memcpy(h80211+len, str, IEEE80211_CHALLENGE_LEN);
    len += 128;

    // FUCK YEAH on commence a avoir qqc.
    // mac address * 3 -> 18 bytes
    // +
    // on peut caller 85 bytes de CHALLENGE
    // -> 18+85 = 103 !!!!

    //memcpy(h80211+len, str, sizeof(str));
    //memcpy(h80211+len, str, sizeof(str));
    len += sizeof(str);


    send_packet(h80211, len);
    send_packet(h80211, len);
    send_packet(h80211, len);
    //h80211[0] = IEEE80211_FC0_TYPE_MGT | IEEE80211_FC0_SUBTYPE_REASSOC_RESP;
    //send_packet(h80211, len);
    //send_packet(h80211, len);
    //send_packet(h80211, len);
    //h80211[0] = IEEE80211_FC0_TYPE_MGT | IEEE80211_FC0_SUBTYPE_PROBE_REQ;
    //send_packet(h80211, len);
    //send_packet(h80211, len);
    //send_packet(h80211, len);
    //h80211[0] = IEEE80211_FC0_TYPE_MGT | IEEE80211_FC0_SUBTYPE_PROBE_RESP;
    //send_packet(h80211, len);
    //send_packet(h80211, len);
    //send_packet(h80211, len);
    //h80211[0] = IEEE80211_FC0_TYPE_MGT | IEEE80211_FC0_SUBTYPE_BEACON;
    //send_packet(h80211, len);
    //send_packet(h80211, len);
    //send_packet(h80211, len);
    //h80211[0] = IEEE80211_FC0_TYPE_MGT | IEEE80211_FC0_SUBTYPE_ATIM;
    //send_packet(h80211, len);
    //send_packet(h80211, len);
    //send_packet(h80211, len);
    //h80211[0] = IEEE80211_FC0_TYPE_MGT | IEEE80211_FC0_SUBTYPE_DISASSOC;
    //send_packet(h80211, len);
    //send_packet(h80211, len);
    //send_packet(h80211, len);
    //h80211[0] = IEEE80211_FC0_TYPE_MGT | IEEE80211_FC0_SUBTYPE_AUTH;
    //send_packet(h80211, len);
    //send_packet(h80211, len);
    //send_packet(h80211, len);
    //h80211[0] = IEEE80211_FC0_TYPE_MGT | IEEE80211_FC0_SUBTYPE_DEAUTH;
    //send_packet(h80211, len);
    //send_packet(h80211, len);
    //send_packet(h80211, len);


    return 0;
}

int main( int argc, char *argv[] )
{
    int n, i, ret;

    /* check the arguments */

    memset( &opt, 0, sizeof( opt ) );
    memset( &dev, 0, sizeof( dev ) );

    opt.f_minlen  = -1; opt.f_maxlen    = -1;
    opt.f_tods    = -1; opt.f_fromds    = -1;

    while( 1 )
    {
        int option_index = 0;

        static struct option long_options[] = {
            {"help",        0, 0, 'H'},
            {0,             0, 0,  0 }
        };

        int option = getopt_long( argc, argv,
                        "H",
                        long_options, &option_index );

        if( option < 0 ) break;

        switch( option )
        {
            case 0 :

                break;

            case ':' :

                printf("\"%s --help\" for help.\n", argv[0]);
                return( 1 );

            case '?' :

                printf("\"%s --help\" for help.\n", argv[0]);
                return( 1 );

            case 'H' :

                printf( "%s", usage );
                return( 1 );

            default : goto usage;
        }
    }

    if( argc - optind != 1 )
    {
        if(argc == 1)
        {
usage:
            printf( "%s", usage );
        }
        if( argc - optind == 0)
        {
            printf("No replay interface specified.\n");
        }
        if(argc > 1)
        {
            printf("\"%s --help\" for help.\n", argv[0]);
        }
        return( 1 );
    }

    if( (opt.f_minlen > 0 && opt.f_maxlen > 0) && opt.f_minlen > opt.f_maxlen )
    {
        printf( "Invalid length filter (min(-m):%d > max(-n):%d).\n",
                opt.f_minlen, opt.f_maxlen );
        printf("\"%s --help\" for help.\n", argv[0]);
        return( 1 );
    }

    if ( opt.f_tods == 1 && opt.f_fromds == 1 )
    {
        printf( "FromDS and ToDS bit are set: packet has to come from the AP and go to the AP\n" );
    }

    /* open the RTC device if necessary */


    opt.iface_out = argv[optind];
    opt.port_out = get_ip_port(opt.iface_out, opt.ip_out, sizeof(opt.ip_out)-1);

    //don't open interface(s) when using test mode and airserv
    if (1)
    {
        /* open the replay interface */
        _wi_out = wi_open(opt.iface_out);
        if (!_wi_out)
            return 1;
        dev.fd_out = wi_fd(_wi_out);

        /* open the packet source */
        {
            _wi_in = _wi_out;
            dev.fd_in = dev.fd_out;

            /* XXX */
            dev.arptype_in = dev.arptype_out;
            wi_get_mac(_wi_in, dev.mac_in);
        }

        wi_get_mac(_wi_out, dev.mac_out);
    }

    /* drop privileges */
    if (setuid( getuid() ) == -1) {
        perror("setuid");
    }

    return do_attack_test();
}

/**********************                           mod_analytics lighttpd module                                             			     ************************/
/**********************     On startup this module will check whether the daemon which will collect data is configured. If yes, this will    ************************/
/**********************     send logs of all the content served by lighttpd to the daemon. During its runtime        					     ************************/
/**********************     if connection drops for some reasons, it will try to reconnect based on a timestamp           					 ************************/
/**********************     it attempted to connect before. The timestamp will increase exponentially if it keeps .       					 ************************/
/**********************     on failing. This will reduce connect and send calls thereby keeping this module light weight  					 ************************/


#include "base.h"
#include "log.h"
#include "buffer.h"

#include "plugin.h"
#include "inet_ntop_cache.h"

#include "pmessage.pb-c.h"

#include <ctype.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>

typedef struct {
	PLUGIN_DATA;
	int sockfd;
	char *socket_path;
	unsigned int time_stamp;
	/* paxus socket reconnect delay in sec   */
	unsigned int retry_delay; 
	unsigned int paxus_cfg;
	unsigned int connected;
	struct sockaddr_un addr;
	
} plugin_data;

static void establish_connection(plugin_data *p)
{
	int flags  = 0;
	if(NULL == p)
	{
		p->connected = 0;
		return;
	}

	
	/************** Initialize a domain socket which will send the structure to Paxus ******************/

	if ( ( p->sockfd = socket ( AF_UNIX, SOCK_STREAM, 0 ) ) == -1 ){
		perror( "socket error" );
		p->connected = 0;
		return;
	}

	flags = fcntl(p->sockfd,F_GETFL,0);
	
   if(flags != -1)
   	fcntl(p->sockfd, F_SETFL, flags | O_NONBLOCK);

	memset( &p->addr, 0, sizeof( p->addr ) );
	p->addr.sun_family = AF_UNIX;
	strncpy( p->addr.sun_path, p->socket_path, sizeof( p->addr.sun_path ) - 1 );
	
	/****************** connect to the socket   **********************************************/
	
	if ( connect ( p->sockfd, ( struct sockaddr* ) &p->addr, sizeof( p->addr ) ) == -1 )
	{
	  	perror ( "mod_analytics.so:connect error" );
	  	p->connected = 0;
	  	 
    	/****   Record initial timestamp when connect failed. Check for timestamp when trying to connect again from REQUEST_DONE func   ********/

	  	p->time_stamp = (int)time(NULL);
	  	p->retry_delay *= 2;
	  	p->time_stamp +=p->retry_delay ;
	}
	else{
	 	p->time_stamp = 0;
	  	p->connected = 1;
	  	p->retry_delay = 5;
	}
}


INIT_FUNC(mod_paxus_init) {
	plugin_data *p;
	p = calloc ( 1, sizeof(*p));
	if(p)
	{
		p->connected = 0;
		p->time_stamp = 0;
		p->retry_delay = 5;  
		p->paxus_cfg = 0;
		if (access ("/etc/lru/components/daemon.cfg", F_OK ) != -1)
		{
			p->paxus_cfg = 1;
			p->socket_path = getenv("DAEMON_ACCESS_SOCK");
			if (p->socket_path==NULL)
			{
				perror( "cannot find environment variable for socket file using default path");
				p->socket_path = "/var/spool/plugins/daemonlogpipe";
			}
			establish_connection(p);
		}
	}
	return p;
}

FREE_FUNC ( mod_paxus_free )
{
	plugin_data *p = p_d;
	UNUSED(srv);
	if(p)
	{
		close(p->sockfd);
		free(p);

	}
	return HANDLER_GO_ON;
}

REQUESTDONE_FUNC(log_pana_write) {

	int n;
	unsigned int *length;
	plugin_data *p = p_d;
	
   PMessage msg = PMESSAGE__INIT;
   void *buf = NULL;
   unsigned int len;
   size_t ret;
	data_string *ds;
	if(NULL == p)
	{
		return HANDLER_GO_ON;
	}
	/**************   Check if exw_paxus exists on LRU     *************************************************/
	if(p->paxus_cfg)
	{
	
		/**************         HTTP status code       *****************************************************/

		msg.status = con->http_status;

		/**************       	Timestamp                  **************************************************/

		msg.timestamp = ctime(&(srv->cur_ts));

		/**************         Address of the remote host         *****************************************/

		msg.destination_address = (char *)inet_ntop_cache_get_ip (srv, &(con->dst_addr));

		/**************        path of the file served     *************************************************/

		if (con->physical.path->used > 1) 
		{
			msg.physical_path = con->physical.path->ptr;
		} 
		else 
		{
			msg.physical_path = NULL;
		}
		/**************          User Agent          *****************************************************/

		if(NULL != (ds = (data_string *) array_get_element(con->request.headers, "User-Agent")))
		{
			msg.user_agent = ds->value->ptr;
		}
		else
		{
			msg.user_agent = NULL;
		}
		/*************          Byte range           *****************************************************/
		if(NULL != (ds = (data_string *) array_get_element(con->response.headers, "Content-Range")))
		{
			msg.content_range = ds->value->ptr;
			
		}

		/************            Request URI         *****************************************************/

		msg.bytes_header = con->uri.path_raw->ptr;
		
		/**************         Bytes written          *****************************************************/

		if (con->bytes_written > 0)
		{
			msg.bytes_written = con->bytes_written;
		} 
		else 
		{
			msg.bytes_written = 0;
		}

		/**************            open connections with server   ******************************************/

		msg.connections = srv->conns->used; 

		/***************** Pack the message using protobuf-c      ******************************************/     

		len = pmessage__get_packed_size(&msg);
		if(len <= 0)
		{
			return HANDLER_GO_ON;
		}

		buf = malloc (len);
		if(NULL == buf)
		{
			return HANDLER_GO_ON;
		}

		ret = pmessage__pack(&msg, buf);
		if(ret <= 0)
		{
			return HANDLER_GO_ON;
		}
		length = &len;

		/***************** Send the structure over domain socket to paxus  ****************************/
		if(!p->connected)
		{
			if (p->time_stamp < (unsigned int)time(NULL))
			{
				/*log_error_write(srv, __FILE__, __LINE__, "s", "Not Connected .. Reconnecting to send to paxususageserv "); */

				if ( connect ( p->sockfd, ( struct sockaddr* ) &p->addr, sizeof( p->addr ) ) == -1 )
				{
					p->connected = 0;
					/*** Record the timestamp when connect failed. Double the retry_delay   ***/
					p->retry_delay = p->retry_delay * 2;
					p->time_stamp += p->retry_delay;

					/*log_error_write(srv, __FILE__, __LINE__, "ss", "connection failed.",strerror(errno)); */
				}
				else
				{
					/** connection established. Reset the timestamp to current. Reset the retry_delay value to 5 seconds   *********/	
					p->connected = 1;
					p->time_stamp = 0;
					p->retry_delay = 5;
				}
			} 
		} 

		if (p->connected)
		{
			n = send ( p->sockfd, (void *)length, sizeof(unsigned int), MSG_NOSIGNAL );
			
			if(n < 0)
			{
				log_error_write(srv, __FILE__, __LINE__, "ss", " Error in sending data to Daemon",strerror(errno));
				/***  Send call failed because the fd was not avaiable. Create a new fd. It will never send       *******/
				/***  unless a connection is established again. The new connection will be established based on time stamp from this function itself***/

				if(errno == EBADF || errno == EPIPE || errno == ENOTSOCK)
				{
					close(p->sockfd);
					establish_connection(p);
				}
				free(buf);
				return HANDLER_GO_ON;
			}

			n = send ( p->sockfd, buf, len, MSG_NOSIGNAL );   

			if(n < 0)
			{
				log_error_write(srv, __FILE__, __LINE__, "ss", " Error in sending data to Daemon",strerror(errno));
				/***  Send call failed because the fd was not avaiable. Create a new fd. It will never send       *******/
				/***  unless a connection is established again. The new connection will be established based on time stamp from this function itself***/

				if(errno == EBADF || errno == EPIPE || errno == ENOTSOCK)
				{
					close(p->sockfd);
					establish_connection(p);
				}
			}

		}
		free(buf);
	}
	
   return HANDLER_GO_ON;
}

int mod_paxus_plugin_init(plugin *p);
int mod_paxus_plugin_init(plugin *p) {
	p->version     = LIGHTTPD_VERSION_ID;
	p->name        = buffer_init_string("paxus_lightty_mod");

	p->init        = mod_paxus_init;
	p->cleanup     = mod_paxus_free;

	p->handle_request_done = log_pana_write;
	p->data        = NULL;

	return 0;
}

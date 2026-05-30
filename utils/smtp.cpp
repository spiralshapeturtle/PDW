// 
/*
**	SMTP routines for mailsend - a simple mail sender via SMTP
**
*/

#include <windows.h>
#include <stdio.h>
#include "..\headers\pdw.h"
#include "smtp_int.h"
#include "smtp.h"
#include "..\utils\debug.h"

#include "openssl\ssl.h"
#include "openssl\err.h"
#include "openssl\x509.h"
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")

// FIX [L3]: increased from 1024 — verbose EHLO replies can exceed 1 kB
#define MY_BUFF_SIZE 4096

static SOCKET smtp_socket = INVALID_SOCKET;
static char buf[MY_BUFF_SIZE];

static HANDLE MailThread ;
static HANDLE hMailEvent = NULL ;
static SOCKET g_persistSocket = INVALID_SOCKET ;
static THEMAIL mail ;
static int nMaxLen ;
static BOOL keepbusy = TRUE ;
static BOOL bWsaStartup ;

#define MAX_MAIL		100
#define MAX_MAIL_LEN	1024

// FIX [MailSplit]: separator byte carrying Subject vs Body through the mail queue.
// 0x1F (US, unit separator) never occurs in pager text, so it is a safe delimiter.
#define MAIL_SPLIT_SEP '\x1f'

static char szMailBuffer[MAX_MAIL][MAX_MAIL_LEN] ;
static int  nBufferdMailStart ;
static int  nBufferdMailEnd ;

static byte dtable[256];

extern int nSMTPerrors;
extern int iSMTPlastError;
extern PROFILE Profile;	// FIX [SmtpLog]: needed for LogfilePath
extern char szPath[];		// FIX [SmtpLog]: needed for disk log path

// FIX [SmtpLog]: global buffer for last SMTP error (user-readable) — displayed in Setup dialog
static char g_szLastSmtpError[512] = "";

//SSL
SSL_CTX*      m_ctx;
SSL*          m_ssl;


char *szSmtpCharSets[] = {
	"us-ascii     (Standard)",
	"iso-8859-1   (West European)",
	"iso-8859-2   (East European)",
	"iso-8859-3   (South European)", 
	"iso-8859-4   (North European)",
	"iso-8859-5   (Cyrillic)",
	"iso-8859-6   (Arabic)", 
	"iso-8859-7   (Greek)", 
	"iso-8859-8   (Hebrew)",
	"iso-8859-9   (Turkish)",
	"iso-8859-10  (Nordic)",
	"iso-2022-kr  (Korean)",
	"KOI8-R       (Russian)",
	"EUC-KR       (Korean)",
	"Shift_JIS    (Japanese)",
	"ISO-2022-JP  (Japanese)",
	"EUC-JP       (Japanese)",
	"GB2312       (Chinese)",
	"Big5         (Traditional Chinese)",
	"windows-1250 (Central Europ Windows)",
	"windows-1251 (Cyrillic Windows)",
	"windows-1252 (Western Europ Windows)",
	"windows-1253 (Greek Windows)",
	"windows-1254 (Turkish (Windows)",
	"windows-1255 (Hebrew Windows)",
	"windows-1256 (Arabic Windows)",
	"windows-1257 (Baltic Windows)",
	"windows-1258 (Vietnamese Windows)"
} ;

enum SSLError
{
	CSMTP_NO_ERROR = 0,
	WSA_STARTUP = 100, // WSAGetLastError()
	WSA_VER,
	WSA_SEND,
	WSA_RECV,
	WSA_CONNECT,
	WSA_GETHOSTBY_NAME_ADDR,
	WSA_INVALID_SOCKET,
	WSA_HOSTNAME,
	WSA_IOCTLSOCKET,
	WSA_SELECT,
	BAD_IPV4_ADDR,
	UNDEF_MSG_HEADER = 200,
	UNDEF_MAIL_FROM,
	UNDEF_SUBJECT,
	UNDEF_RECIPIENTS,
	UNDEF_LOGIN,
	UNDEF_PASSWORD,
	BAD_LOGIN_PASSWORD,
	BAD_DIGEST_RESPONSE,
	BAD_SERVER_NAME,
	UNDEF_RECIPIENT_MAIL,
	COMMAND_MAIL_FROM = 300,
	COMMAND_EHLO,
	COMMAND_AUTH_PLAIN,
	COMMAND_AUTH_LOGIN,
	COMMAND_AUTH_CRAMMD5,
	COMMAND_AUTH_DIGESTMD5,
	COMMAND_DIGESTMD5,
	COMMAND_DATA,
	COMMAND_QUIT,
	COMMAND_RCPT_TO,
	MSG_BODY_ERROR,
	CONNECTION_CLOSED = 400, // by server
	SERVER_NOT_READY, // remote server
	SERVER_NOT_RESPONDING,
	SELECT_TIMEOUT,
	FILE_NOT_EXIST,
	MSG_TOO_BIG,
	BAD_LOGIN_PASS,
	UNDEF_XYZ_RESPONSE,
	LACK_OF_MEMORY,
	TIME_ERROR,
	RECVBUF_IS_EMPTY,
	SENDBUF_IS_EMPTY,
	OUT_OF_MSG_RANGE,
	COMMAND_EHLO_STARTTLS,
	SSL_PROBLEM,
	COMMAND_DATABLOCK,
	STARTTLS_NOT_SUPPORTED,
	LOGIN_NOT_SUPPORTED
};

// FIX [SmtpTLS]: accept hostname so SNI and cert verification can be set per-connect
static char g_szTlsHostname[256] = "";

int initOpenSSL()
{
	SSL_library_init();
	SSL_load_error_strings();
	m_ctx = SSL_CTX_new (TLS_client_method());
	if(m_ctx == NULL)
		return SSL_PROBLEM;

	// FIX [SmtpTLS]: enforce TLS 1.2 minimum; disable broken legacy versions
	SSL_CTX_set_min_proto_version(m_ctx, TLS1_2_VERSION);

	// FIX [SmtpTLS]: require certificate verification
	SSL_CTX_set_verify(m_ctx, SSL_VERIFY_PEER, NULL);

	// FIX [SmtpTLS]: load system CA store so Windows-trusted certificates are accepted
	HCERTSTORE hStore = CertOpenSystemStoreA(0, "ROOT");
	if (hStore) {
		X509_STORE *x509Store = SSL_CTX_get_cert_store(m_ctx);
		PCCERT_CONTEXT pCert  = NULL;
		while ((pCert = CertEnumCertificatesInStore(hStore, pCert)) != NULL) {
			const unsigned char *p = pCert->pbCertEncoded;
			X509 *x = d2i_X509(NULL, &p, (long)pCert->cbCertEncoded);
			if (x) { X509_STORE_add_cert(x509Store, x); X509_free(x); }
		}
		CertCloseStore(hStore, 0);
	}

	return CSMTP_NO_ERROR;
}


#define TIME_IN_SEC		3*60	// how long client will wait for server response in non-blocking mode

int openSSLConnect()
{
	if(m_ctx == NULL)
		return SSL_PROBLEM;

	m_ssl = SSL_new (m_ctx);
	if(m_ssl == NULL)
		return SSL_PROBLEM;

	SSL_set_fd (m_ssl, (int)smtp_socket);
	SSL_set_mode(m_ssl, SSL_MODE_AUTO_RETRY);

	// FIX [SmtpTLS]: set SNI hostname so virtual-hosted servers send the right cert
	if (g_szTlsHostname[0])
		SSL_set_tlsext_host_name(m_ssl, g_szTlsHostname);

	// FIX [SmtpTLS]: enable hostname verification against the peer certificate
	if (g_szTlsHostname[0])
		SSL_set1_host(m_ssl, g_szTlsHostname);

	int res = 0;
	fd_set fdwrite;
	fd_set fdread;
	int write_blocked = 0;
	int read_blocked = 0;

	timeval time;
	time.tv_sec = TIME_IN_SEC;
	time.tv_usec = 0;

	while(1)
	{
		FD_ZERO(&fdwrite);
		FD_ZERO(&fdread);

		if(write_blocked)
			FD_SET(smtp_socket, &fdwrite);
		if(read_blocked)
			FD_SET(smtp_socket, &fdread);

		if(write_blocked || read_blocked)
		{
			write_blocked = 0;
			read_blocked = 0;
			if((res = select(smtp_socket+1,&fdread,&fdwrite,NULL,&time)) == SOCKET_ERROR)
			{
				FD_ZERO(&fdwrite);
				FD_ZERO(&fdread);
				return WSA_SELECT;
			}
			if(!res)
			{
				//timeout
				FD_ZERO(&fdwrite);
				FD_ZERO(&fdread);
				return SERVER_NOT_RESPONDING;
			}
		}
		res = SSL_connect(m_ssl);
		switch(SSL_get_error(m_ssl, res))
		{
		case SSL_ERROR_NONE:
			FD_ZERO(&fdwrite);
			FD_ZERO(&fdread);
			return CSMTP_NO_ERROR;
			break;

		case SSL_ERROR_WANT_WRITE:
			write_blocked = 1;
			break;

		case SSL_ERROR_WANT_READ:
			read_blocked = 1;
			break;

		default:	      
			FD_ZERO(&fdwrite);
			FD_ZERO(&fdread);
			return SSL_PROBLEM;
		}
	}

	return CSMTP_NO_ERROR;
}


void cleanupOpenSSL()
{
	if(m_ssl != NULL)
	{
		SSL_shutdown (m_ssl);  /* send SSL/TLS close_notify */
		SSL_free (m_ssl);
		m_ssl = NULL;
	}
	if(m_ctx != NULL)
	{
		SSL_CTX_free (m_ctx);	
		m_ctx = NULL;
	}
}


#define SEND_RECIEVE_TO 5*60

int receiveData_SSL(SSL* ssl, char* buf)
{
	int res = 0;
	int offset = 0;
	fd_set fdread;
	fd_set fdwrite;
	timeval time;

	int read_blocked_on_write = 0;

	time.tv_sec = SEND_RECIEVE_TO;
	time.tv_usec = 0;

	if(buf == NULL)
		return RECVBUF_IS_EMPTY;

	bool bFinish = false;

	while(!bFinish)
	{
		FD_ZERO(&fdread);
		FD_ZERO(&fdwrite);

		FD_SET(smtp_socket,&fdread);

		if(read_blocked_on_write)
		{
			FD_SET(smtp_socket, &fdwrite);
		}

		if((res = select(smtp_socket+1, &fdread, &fdwrite, NULL, &time)) == SOCKET_ERROR)
		{
			FD_ZERO(&fdread);
			FD_ZERO(&fdwrite);
			return WSA_SELECT;
		}

		if(!res)
		{
			//timeout
			FD_ZERO(&fdread);
			FD_ZERO(&fdwrite);
			return SERVER_NOT_RESPONDING;
		}

		if(FD_ISSET(smtp_socket,&fdread) || (read_blocked_on_write && FD_ISSET(smtp_socket,&fdwrite)) )
		{
			while(1)
			{
				read_blocked_on_write=0;

				const int buff_len = 1024;
				char buff[buff_len];

				res = SSL_read(ssl, buff, buff_len);

				int ssl_err = SSL_get_error(ssl, res);
				if(ssl_err == SSL_ERROR_NONE)
				{
					if(offset + res > MY_BUFF_SIZE - 1)
					{
						FD_ZERO(&fdread);
						FD_ZERO(&fdwrite);
						return LACK_OF_MEMORY;
					}
					memcpy(buf + offset, buff, res);
					offset += res;
					if(SSL_pending(ssl))
					{
						continue;
					}
					else
					{
						bFinish = true;
						break;
					}
				}
				else if(ssl_err == SSL_ERROR_ZERO_RETURN)
				{
					bFinish = true;
					break;
				}
				else if(ssl_err == SSL_ERROR_WANT_READ)
				{
					break;
				}
				else if(ssl_err == SSL_ERROR_WANT_WRITE)
				{
					/* We get a WANT_WRITE if we're
					trying to rehandshake and we block on
					a write during that rehandshake.

					We need to wait on the socket to be 
					writeable but reinitiate the read
					when it is */
					read_blocked_on_write=1;
					break;
				}
				else
				{
					FD_ZERO(&fdread);
					FD_ZERO(&fdwrite);
					return SSL_PROBLEM;
				}
			}
		}
	}

	FD_ZERO(&fdread);
	FD_ZERO(&fdwrite);
	buf[offset] = 0;
	if(offset == 0)
	{
		return CONNECTION_CLOSED;
	}

	return CSMTP_NO_ERROR;
}

int sendData_SSL(SSL* ssl, char *buf)
{
	int offset = 0,res,nLeft = strlen(buf);
	fd_set fdwrite;
	fd_set fdread;
	timeval time;

	int write_blocked_on_read = 0;

	time.tv_sec = SEND_RECIEVE_TO; 
	time.tv_usec = 0;


	if(buf == NULL)
		return SENDBUF_IS_EMPTY;

	while(nLeft > 0)
	{
		FD_ZERO(&fdwrite);
		FD_ZERO(&fdread);

		FD_SET(smtp_socket,&fdwrite);

		if(write_blocked_on_read)
		{
			FD_SET(smtp_socket, &fdread);
		}

		if((res = select(smtp_socket+1,&fdread,&fdwrite,NULL,&time)) == SOCKET_ERROR)
		{
			FD_ZERO(&fdwrite);
			FD_ZERO(&fdread);
			return WSA_SELECT;
		}

		if(!res)
		{
			//timeout
			FD_ZERO(&fdwrite);
			FD_ZERO(&fdread);
			return SERVER_NOT_RESPONDING;
		}

		if(FD_ISSET(smtp_socket,&fdwrite) || (write_blocked_on_read && FD_ISSET(smtp_socket, &fdread)) )
		{
			write_blocked_on_read=0;

			/* Try to write */
			res = SSL_write(ssl, buf+offset, nLeft);
	          
			switch(SSL_get_error(ssl,res))
			{
			  /* We wrote something*/
			  case SSL_ERROR_NONE:
				nLeft -= res;
				offset += res;
				break;
	              
				/* We would have blocked */
			  case SSL_ERROR_WANT_WRITE:
				break;

				/* We get a WANT_READ if we're
				   trying to rehandshake and we block on
				   write during the current connection.
	               
				   We need to wait on the socket to be readable
				   but reinitiate our write when it is */
			  case SSL_ERROR_WANT_READ:
				write_blocked_on_read=1;
				break;
	              
				  /* Some other error */
			  default:	      
				FD_ZERO(&fdread);
				FD_ZERO(&fdwrite);
				return SSL_PROBLEM;
			}

		}
	}

	OutputDebugStringA(buf);
	FD_ZERO(&fdwrite);
	FD_ZERO(&fdread);

	return CSMTP_NO_ERROR;
}
	
char *EncodeBase64(char *szIn, char *szOut)
{
	char *pIn = szIn, *pOut = szOut ;
	int i,hiteof= FALSE;

	for(i= 0;i<9;i++){
		dtable[i]= 'A'+i;
		dtable[i+9]= 'J'+i;
		dtable[26+i]= 'a'+i;
		dtable[26+i+9]= 'j'+i;
	}
	for(i= 0;i<8;i++){
		dtable[i+18]= 'S'+i;
		dtable[26+i+18]= 's'+i;
	}
	for(i= 0;i<10;i++){
		dtable[52+i]= '0'+i;
	}
	dtable[62]= '+';
	dtable[63]= '/';


	while(!hiteof){
		byte igroup[3],ogroup[4];
		int c,n;
	
		igroup[0]= igroup[1]= igroup[2]= 0;
		for(n= 0;n<3;n++){
			c = *pIn++;
			if(!c){
				hiteof= TRUE;
				break;
			}
			igroup[n]= (byte)c;
		}
		if(n> 0){
			ogroup[0]= dtable[igroup[0]>>2];
			ogroup[1]= dtable[((igroup[0]&3)<<4)|(igroup[1]>>4)];
			ogroup[2]= dtable[((igroup[1]&0xF)<<2)|(igroup[2]>>6)];
			ogroup[3]= dtable[igroup[2]&0x3F];

			if(n<3){
				ogroup[3]= '=';
				if(n<2){
					ogroup[2]= '=';
				}
			}
			for(i= 0;i<4;i++){
				*pOut++ = ogroup[i];
			}
		}
	}
	*pOut = '\0' ;

	OUTPUTDEBUGMSG((("EncodeBase64(): In >%s< out >%s< \n"),szIn, szOut));
	return(szOut) ;
}

char *DecodeBase64(char *szIn, char *szOut)
{
	int i, j;
	char *pIn = szIn, *pOut = szOut ;

	for(i= 0;i<255;i++){
		dtable[i]= 0x80;
	}
	for(i= 'A';i<='I';i++){
		dtable[i]= 0+(i-'A');
	}
	for(i= 'J';i<='R';i++){
		dtable[i]= 9+(i-'J');
	}
	for(i= 'S';i<='Z';i++){
		dtable[i]= 18+(i-'S');
	}
	for(i= 'a';i<='i';i++){
		dtable[i]= 26+(i-'a');
	}
	for(i= 'j';i<='r';i++){
		dtable[i]= 35+(i-'j');
	}
	for(i= 's';i<='z';i++){
		dtable[i]= 44+(i-'s');
	}
	for(i= '0';i<='9';i++){
		dtable[i]= 52+(i-'0');
	}
	dtable['+']= 62;
	dtable['/']= 63;
	dtable['=']= 0;

	while(TRUE){
		byte a[4],b[4],o[3];
		
		for(i = 0; i < 4; i++){
			int c = *pIn++;		
			if(!c){
				if(i> 0){
					OUTPUTDEBUGMSG((("DecodeBase64(): Input line incomplete.\n")));
				}
				*pOut = '\0'  ;
				OUTPUTDEBUGMSG((("DecodeBase64(): In >%s< out >%s< \n"),szIn, szOut));
				return(szOut);
			}
			if(dtable[c]&0x80){
				OUTPUTDEBUGMSG((("DecodeBase64(): Illegal character '%c' in input line.\n"),c));
				i--;
				continue;
			}
			a[i]= (byte)c;
			b[i]= (byte)dtable[c];
		}
		o[0]= (b[0]<<2)|(b[1]>>4);
		o[1]= (b[1]<<4)|(b[2]>>2);
		o[2]= (b[2]<<6)|b[3];
		i = a[2]=='='?1:(a[3]=='='?2:3);

		for(j = 0; j < i; j++) {
			*pOut++ = o[j] ;
		}
		if(i < 3){
			*pOut = '\0'  ;
			OUTPUTDEBUGMSG((("DecodeBase64(): In >%s< out >%s< \n"),szIn, szOut));
			return(szOut);
		}
	}	
}

void AddResponse(char *buf)
{
// #ifdef _DEBUG
	HDC		hDC ;
	SIZE	Size ;

	if(mail.hResponse) {
		hDC = GetDC(mail.hResponse) ;
		GetTextExtentPoint32(hDC, buf, strlen(buf), &Size);
		if(Size.cx > nMaxLen) {
			nMaxLen = Size.cx ;
			SendMessage(mail.hResponse, LB_SETHORIZONTALEXTENT, Size.cx, 0L) ;
		}
		SendMessage(mail.hResponse, LB_ADDSTRING, 0, (LPARAM) (LPSTR) buf) ;
		OUTPUTDEBUGMSG((("AddResponse() : >>> %s"),buf));
	}

	// FIX [SmtpLog]: log all SMTP responses to disk if checkbox enabled
	if (Profile.bMailLogErrors) {
		char szLog[MAX_PATH];
		const char *root = (Profile.LogfilePath[0]) ? Profile.LogfilePath : szPath;
		if (root && root[0]) {
			_snprintf(szLog, sizeof(szLog) - 1, "%s\\pdw_smtp_error.log", root);
			szLog[sizeof(szLog) - 1] = '\0';

			FILE *f = fopen(szLog, "a");
			if (f) {
				SYSTEMTIME st;
				GetLocalTime(&st);
				fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d | %s\n",
					st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, buf);
				fclose(f);
			}
		}
	}
// #endif
}


struct in_addr *atoAddr(char *address)
{
	static struct in_addr saddr;
	struct hostent *host;
	
	saddr.s_addr=inet_addr(address);
	if(saddr.s_addr != -1) return (&saddr);
	host = gethostbyname(address);
	if(host != (struct hostent *) NULL) {
		return((struct in_addr *) *host->h_addr_list);
	}
	return((struct in_addr *) NULL);
}

int initWinSock(void)
{
	WORD	version_requested;
	WSADATA wsa_data;
	int		err;
	
	if(!bWsaStartup) {
		version_requested=MAKEWORD(2,0);
		err = WSAStartup(version_requested,&wsa_data);
		if(err != 0) {
			OUTPUTDEBUGMSG(((" Unable to initialize winsock (%d)\n"),err));
			AddResponse("Unable to initialize winsock\n");
			bWsaStartup = FALSE ;
			return(-1);
		}
		bWsaStartup = TRUE ;
	}
	return(0);
}

// returns SOCKET on success INVALID_SOCKET on failure
SOCKET clientSocket(char *address,int port)
{
	SOCKET				s;
	struct sockaddr_in	sa;
	struct in_addr		*addr;
	int 				rc;
	
	rc = initWinSock();
	if(rc != 0) {
		OUTPUTDEBUGMSG((("clientSocket() : Error in initWinSock() = 0x%04x\n"), rc));
		AddResponse("clientSocket() : Error in initWinSock()\n");
		return(INVALID_SOCKET);
	}	
	addr = atoAddr(address);
	if(addr == NULL) {
		OUTPUTDEBUGMSG((("clientSocket() : Invalid address: %s\n"),address));
		AddResponse("clientSocket() : Invalid address\n");
		return(INVALID_SOCKET);
	}
	
	memset((char *) &sa,0,sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((unsigned short) port);
	sa.sin_addr.s_addr = addr->s_addr;
	
	// open the socket
	s = socket(AF_INET,SOCK_STREAM,PF_UNSPEC);
	if(s == INVALID_SOCKET) {
		OUTPUTDEBUGMSG((("clientSocket() : Could not create socket\n")));
		AddResponse("clientSocket() : Could not create socket\n");
		return(INVALID_SOCKET);
	}
	// FIX [SmtpConnect]: check connect() return; previous code silently returned a bad socket
	if (connect(s, (struct sockaddr *) &sa, sizeof(sa)) == SOCKET_ERROR) {
		OUTPUTDEBUGMSG((("clientSocket() : connect() failed WSAError=%d\n"), WSAGetLastError()));
		AddResponse("clientSocket() : connect() failed\n");
		closesocket(s);
		return(INVALID_SOCKET);
	}
	return(s);
}




// this function writes a character string out to a socket.
// it returns -1 if the connection is closed while it is trying to write
static int sockWrite(SOCKET sock,char *str,size_t count)
{
	size_t	bytesSent=0 ; 
	int		thisWrite ;
	
	while (bytesSent < count)  {
		thisWrite=send(sock,str,count-bytesSent,0) ;
		if(thisWrite <= 0) {
			return (thisWrite) ;
		}
		bytesSent += thisWrite ; 
		str += thisWrite ;
	}
	return(count) ;
}

int sockPuts(SOCKET sock,char *str)
{
// 	OUTPUTDEBUGMSG((("sockPuts() : %s\n"), str));
	AddResponse(str) ;

	if (m_ssl != NULL)
		return sendData_SSL(m_ssl,str);

	return(sockWrite(sock,str,strlen(str)));
}

// FIX [M5]: send without echoing to the response listbox — used for AUTH credentials
static int sockPutsSilent(SOCKET sock, const char *str)
{
	if (m_ssl != NULL)
		return sendData_SSL(m_ssl, (char *)str);
	return sockWrite(sock, (char *)str, strlen(str));
}

int sockGets(SOCKET sockfd,char *str,size_t count)
{
	int bytesRead;
	size_t totalCount = 0;
	char buf[1], *currentPosition;
	char lastRead = 0;

	currentPosition = str;
	while(lastRead != 10) {
		bytesRead=recv(sockfd,buf,1,0);
		OUTPUTDEBUGMSG((("%s,%d\n"), __FILE__, bytesRead));
		if(bytesRead <= 0) {
			// the other side may have closed unexpectedly
			OUTPUTDEBUGMSG((("ERRNO:%d\n"), WSAGetLastError()));
			return (-1);
		}
		lastRead=buf[0];
		if((totalCount < count) && (lastRead != 10) && (lastRead != 13)) {
			*currentPosition=lastRead;
			currentPosition++;
			totalCount++;
		}
	}
	if (count > 0) {
		*currentPosition=0;
	}
	return (totalCount);
}


// disconnect to SMTP server and returns the socket fd
static void smtpDisconnect(SOCKET sfd)
{
	cleanupOpenSSL();
	closesocket(sfd) ;
}

// FIX [SmtpHelo]: forward-declaratie — smtpResponse() wordt hieronder in smtpConnect()
// aangeroepen maar pas later gedefinieerd (use-before-definition gaf C3861)
static int smtpResponse(int sfd);

// FIX [SmtpStartTls]: poort 465 = impliciete TLS (meteen handshaken); elke andere poort
// met SSL aangevinkt (587/25) = STARTTLS — verbinding blijft plaintext tot na de eerste
// EHLO en wordt dan in smtpHelo() versleuteld. Per connect opnieuw bepaald in smtpConnect().
#define SMTP_IMPLICIT_TLS_PORT 465
static BOOL g_useStartTls = FALSE;

// connect to SMTP server and returns the socket fd
static SOCKET smtpConnect(char *smtp_server,int port)
{
	SOCKET sfd;
	int res;

	sfd = clientSocket(smtp_server,port);
	if(sfd == INVALID_SOCKET) {
		OUTPUTDEBUGMSG((("smtpConnect() : Could not connect to SMTP server \"%s\" at port %d\n"), smtp_server,port));
		AddResponse("smtpConnect() : Could not connect to SMTP server\n");

		nSMTPerrors++;	// PH: Counts # Errors

		return (INVALID_SOCKET);
	}
	// save it. we'll need it to clean up
	smtp_socket = sfd;

	// FIX [SmtpTLS]: save hostname for SNI / cert verification before initOpenSSL()
	_snprintf(g_szTlsHostname, sizeof(g_szTlsHostname) - 1, "%s", smtp_server);
	g_szTlsHostname[sizeof(g_szTlsHostname) - 1] = '\0';

	// FIX [SmtpStartTls]: kies TLS-model op basis van de poort
	g_useStartTls = FALSE;
	if (Profile.ssl) {
		if (port == SMTP_IMPLICIT_TLS_PORT) {
			// impliciete TLS (SMTPS): handshake direct na het TCP-connect
			if ((res = initOpenSSL()) == CSMTP_NO_ERROR)
				res = openSSLConnect();
			OUTPUTDEBUGMSG(("SSL Connect res = %d\n",res));
		} else {
			// STARTTLS (submission 587 e.d.): TLS-upgrade gebeurt in smtpHelo() na de eerste EHLO
			g_useStartTls = TRUE;
		}
	}

	// FIX [SmtpHelo]: lees de 220-greeting hier — anders leest smtpHelo() hem als HELO-antwoord
	// en wordt de echte HELO-response (bijv. 501) pas bij AUTH gelezen → AUTH faalt onterecht
	// (bij STARTTLS wordt deze greeting in plaintext gelezen — correct, TLS komt pas na EHLO)
	smtpResponse(sfd);

	return(sfd);
}

// read SMTP response. returns 0 on success, -1 on failure 
static int smtpResponse(int sfd)
{
	int n, err ;
	char buf[MY_BUFF_SIZE], tmp[MY_BUFF_SIZE] ;

	memset(buf,0,sizeof(buf));

	if (m_ssl != NULL)
		err = receiveData_SSL(m_ssl,buf);     // SSL-pad drained de hele reply al
	else {
		// FIX [SmtpStartTls]: lees de volledige (mogelijk multiline) SMTP-reply.
		// EHLO antwoordt met meerdere "250-..." regels en sluit af met "250 ..." (spatie
		// op positie 3). sockGets() leest één regel; zonder deze lus blijven de overige
		// regels in de TCP-buffer staan en desynchroniseert het volgende commando.
		do {
			memset(buf,0,sizeof(buf));
			n = sockGets(sfd, buf, sizeof(buf)-1);
		} while (n > 0 && strlen(buf) >= 4 && buf[3] == '-');
	}
//	OUTPUTDEBUGMSG((("smtpResponse() : %s\n"),buf));
	AddResponse(buf) ;
	err = atoi(buf) ;
	OUTPUTDEBUGMSG((("smtpResponse(): Err: %d!\n"), err));
	if(err == 334) {
		DecodeBase64(buf+4, tmp) ;
		strcpy(buf+4, tmp) ;
	}

	// FIX [L5]: add parentheses — && binds tighter than ||; 3xx needed A_SPACE check
	if(buf[0] == '1' || buf[0] == '2' || (buf[0] == '3' && buf[3] == A_SPACE)) {
		return (0);
	}
	OUTPUTDEBUGMSG((("smtpResponse(): ERROR!\n")));
	nSMTPerrors++;			// PH: Counts # Errors
	iSMTPlastError = err;	// PH: Last Error
	LogSmtpError(err);		// FIX [SmtpLog]: log to pdw_smtp_error.log

	return (-1);
}

// FIX [SmtpHelo]: bouw een RFC 5321-geldig EHLO-argument.
// Een kale "127.0.0.1" is ongeldig (numeriek = geen domeinnaam én geen address-literal) en
// wordt door strikte servers geweigerd ("501 Please fix your HELO string", o.a. Telenet).
// Voorkeur: (1) door de gebruiker ingesteld domein, anders (2) het lokale socket-IP als
// address-literal "[a.b.c.d]" — syntactisch altijd geldig en door elke server geaccepteerd.
static void smtpBuildHeloArg(char *out, size_t outlen)
{
	// 1. expliciet geconfigureerd domein (maar negeer het kapotte oude bare-IP default)
	if (mail.helo_domain && mail.helo_domain[0] &&
		strcmp(mail.helo_domain, "127.0.0.1") != 0) {
		_snprintf(out, outlen - 1, "%s", mail.helo_domain);
		out[outlen - 1] = '\0';
		return;
	}

	// 2. lokaal IP van de verbonden socket als RFC 5321 address-literal
	struct sockaddr_in sa;
	int salen = sizeof(sa);
	memset(&sa, 0, sizeof(sa));
	if (smtp_socket != INVALID_SOCKET &&
		getsockname(smtp_socket, (struct sockaddr *) &sa, &salen) == 0 &&
		sa.sin_family == AF_INET) {
		unsigned char *ip = (unsigned char *) &sa.sin_addr;
		_snprintf(out, outlen - 1, "[%u.%u.%u.%u]", ip[0], ip[1], ip[2], ip[3]);
		out[outlen - 1] = '\0';
		return;
	}

	// 3. laatste redmiddel — bracketed literal (nooit een kale 127.0.0.1)
	_snprintf(out, outlen - 1, "[127.0.0.1]");
	out[outlen - 1] = '\0';
}

// FIX [SmtpHelo]: EHLO i.p.v. HELO — AUTH LOGIN vereist ESMTP; HELO kent geen extensies.
// FIX [SmtpStartTls]: bij STARTTLS-modus (poort 587 e.d.) wordt na de eerste EHLO de
// verbinding versleuteld en daarna verplicht een tweede EHLO over TLS gestuurd.
static int smtpHelo(int sfd)
{
	char szHelo[300];
	smtpBuildHeloArg(szHelo, sizeof(szHelo));

	_snprintf(buf,sizeof(buf)-1,"EHLO %s\r\n", szHelo);
	sockPuts(sfd,buf);
	if (smtpResponse(sfd))
		return (-1);

	// STARTTLS-upgrade: alleen in STARTTLS-modus en zolang er nog geen TLS-laag is
	if (g_useStartTls && m_ssl == NULL) {
		_snprintf(buf,sizeof(buf)-1,"STARTTLS\r\n");
		sockPuts(sfd,buf);
		if (smtpResponse(sfd))                 // server moet 220 sturen
			return (-1);

		if (initOpenSSL() != CSMTP_NO_ERROR || openSSLConnect() != CSMTP_NO_ERROR) {
			OUTPUTDEBUGMSG((("smtpHelo() : STARTTLS handshake mislukt\n")));
			AddResponse("smtpHelo() : STARTTLS handshake failed\n");
			return (-1);
		}

		// na STARTTLS verplicht opnieuw EHLO, nu over de versleutelde verbinding
		_snprintf(buf,sizeof(buf)-1,"EHLO %s\r\n", szHelo);
		sockPuts(sfd,buf);
		if (smtpResponse(sfd))
			return (-1);
	}

	return (0);
}


// SMTP: Authentication
static int smtpLogin(int sfd)
{
	// FIX [SmtpLogin]: base64 of MAIL_TEXT_LEN(100) bytes needs 136 bytes; 128 was too small
	char szTmp[200] ;


	if(mail.options & MAIL_OPTION_AUTH) {
		_snprintf(buf,sizeof(buf)-1,"AUTH LOGIN\r\n");
		sockPuts(sfd,buf);
		if(smtpResponse(sfd)) return(TRUE) ;

		// FIX [M5]: send credentials silently — sockPuts echoes to response listbox
		_snprintf(buf,sizeof(buf)-1, "%s\r\n", EncodeBase64(mail.user, szTmp));
		AddResponse("[AUTH username - hidden]\n");
		sockPutsSilent(sfd, buf);
		if(smtpResponse(sfd)) return(TRUE) ;

		_snprintf(buf,sizeof(buf)-1, "%s\r\n", EncodeBase64(mail.password, szTmp));
		AddResponse("[AUTH password - hidden]\n");
		sockPutsSilent(sfd, buf);
		return(smtpResponse(sfd));
	}
	return(FALSE) ;
}


static void strip_crlf(char *s)
{
	for (; *s; s++)
		if (*s == '\r' || *s == '\n') *s = ' ';
}

// SMTP: MAIL FROM
static int smtpMailFrom(int sfd)
{
	strip_crlf(mail.from);
	_snprintf(buf,sizeof(buf)-1,"MAIL FROM: <%s>\r\n",mail.from);
//	OUTPUTDEBUGMSG((("smtpMailFrom() : >>> %s"),buf));
	sockPuts(sfd,buf);
	return (smtpResponse(sfd));
}

// SMTP: quit
static int smtpQuit(int sfd)
{
	sockPuts(sfd,"QUIT\r\n");
	return (smtpResponse(sfd));
}

// SMTP: RSET
// aborts current mail transaction and cause both ends to reset
static int smtpRset(int sfd)
{
	sockPuts(sfd,"RSET\r\n");
	return (smtpResponse(sfd));
}


char *StripSpecial(char *szStr)
{
	int len = strlen(szStr) ;

	while(len--) {
		switch(szStr[len]) {
			case ',' :
			case ';' :
			case ' ' :
				szStr[len] = '\0' ;
				break ;
			default:
				return(szStr) ;
		}
	}
	return(szStr) ;
}

// SMTP: RCPT TO
static int smtpRcptTo(int sfd)
{
	static char szTemp[MAX_MAIL * 5] ;
	char *pTmp1 = szTemp, *pTmp2 ;

	strncpy(szTemp, mail.to, (MAX_MAIL * 5) - 1) ;
	strip_crlf(szTemp) ;
	StripSpecial(szTemp) ;

	while(1) {
		pTmp2 = strchr(pTmp1, ';') ;
		if(NULL == pTmp2) {
			pTmp2 = strchr(pTmp1, ',') ;
		}
		if(pTmp2) {
			*pTmp2 = '\0' ;
		}
		// FIX [SmtpLog]: trim leading/trailing whitespace from email address
		while(*pTmp1 && isspace((unsigned char)*pTmp1)) pTmp1++;
		char *pEnd = pTmp1 + strlen(pTmp1) - 1;
		while(pEnd > pTmp1 && isspace((unsigned char)*pEnd)) pEnd--;
		*(pEnd + 1) = '\0';

		_snprintf(buf, sizeof(buf)-1, "RCPT TO: <%s>\r\n", pTmp1);
		sockPuts(sfd,buf);
		if (smtpResponse(sfd) != 0) {
			smtpRset(sfd);
			return (-1);
		}
		if(pTmp2) {
			pTmp1 = pTmp2 ;
			pTmp1++ ;
		}
		else {
			break ;
		}
	}
	return (0);

}

// SMTP: DATA
static int smtpData(int sfd)
{
	sockPuts(sfd,"DATA\r\n");
	return(smtpResponse(sfd));
}

// SMTP: EOM
static int smtpEom(int sfd)
{
	sockPuts(sfd,"\r\n.\r\n");
	return (smtpResponse(sfd));
}

// FIX [SmtpDotStuff]: RFC 5321 §4.5.2 — escape DATA lines starting with '.'.
// FIX [M2b]: RFC 5321 §2.3.8 — normalize bare \n to \r\n in the same pass.
static void smtpDotStuff(char *szOut, size_t outLen, const char *szIn)
{
	const char *p = szIn;
	char *q = szOut, *qEnd = szOut + outLen - 1;
	if (*p == '.' && q < qEnd) *q++ = '.';
	while (*p && q < qEnd) {
		// insert \r before bare \n (not already preceded by \r)
		if (*p == '\n' && (p == szIn || *(p - 1) != '\r') && q + 1 < qEnd)
			*q++ = '\r';
		*q++ = *p;
		// dot-stuff: escape leading dot on the line that follows a newline
		if (*p == '\n' && *(p + 1) == '.' && q < qEnd) *q++ = '.';
		p++;
	}
	*q = '\0';
}

// SMTP: mail
static int smtpMail(int sfd, char *data)
{	
	char szBuffer[128], *pTmp ;
	char szSubject[MAX_MAIL_LEN + 32]="";
	char szBody[MAX_MAIL_LEN + 32]="";
	extern int nSMTPemails;

	if (Profile.bMailSplitConfig)
	{
		// FIX [MailSplit]: data = "<subject>\x1f<body>" — split on the first separator.
		const char *sep = strchr(data, MAIL_SPLIT_SEP) ;
		if (sep)
		{
			size_t sl = (size_t)(sep - data) ;
			if (sl > sizeof(szSubject) - 1) sl = sizeof(szSubject) - 1 ;
			memcpy(szSubject, data, sl) ;
			szSubject[sl] = '\0' ;
			_snprintf(szBody, sizeof(szBody) - 1, "%s", sep + 1) ;
		}
		else
		{
			_snprintf(szBody, sizeof(szBody) - 1, "%s", data) ;	// no separator: treat all as body
		}
	}
	else
	{
		for (int i=0; data[i]!=0; i++)
		{
			if (data[i] == '�')
			{
				strcat(szSubject, " - ");
				strcat(szBody, "\n");
			}
			else
			{
				szSubject[strlen(szSubject)] = data[i];
				szBody[strlen(szBody)] = data[i];
			}
		}
	}

	if (Profile.bMailSplitConfig || (mail.options & MAIL_OPTION_SUBJECT))
	{
		if (szSubject[0])
		{
			strip_crlf(szSubject);
			memset(buf,0,sizeof(buf));
			(void) _snprintf(buf,sizeof(buf)-1,"Subject: %s\r\n", szSubject);
			sockPuts(sfd,buf);

			memset(buf,0,sizeof(buf));
			strcpy(szBuffer, szSmtpCharSets[((Profile.nMailOptions & 0x1F0000) >> 16) -1]) ;
			pTmp = strchr(szBuffer, ' ') ;
			if(pTmp != NULL) {
				*pTmp = '\0' ;
			}
			(void) _snprintf(buf,sizeof(buf)-1,"Content-type: text/plain; charset=\"%s\"\r\n", szBuffer);
			sockPuts(sfd,buf);
		}
	}
	
	// headers
	if(mail.from)
	{
		strip_crlf(mail.from);
		memset(buf,0,sizeof(buf));
		(void) _snprintf(buf,sizeof(buf)-1,"From: %s\r\n",mail.from);
		sockPuts(sfd,buf);
	}
	if(mail.to)
	{
		strip_crlf(mail.to);
		memset(buf,0,sizeof(buf));
		(void) _snprintf(buf,sizeof(buf)-1,"To: %s\r\n",mail.to);
		sockPuts(sfd,buf);
	}

	if(mail.cc)
	{
		strip_crlf(mail.cc);
		memset(buf,0,sizeof(buf));
		(void) _snprintf(buf,sizeof(buf)-1,"Cc: %s\r\n",mail.cc);
		sockPuts(sfd,buf);
	}
	if(mail.bcc)
	{
		strip_crlf(mail.bcc);
		memset(buf,0,sizeof(buf));
		(void) _snprintf(buf,sizeof(buf)-1,"Bcc: %s\r\n",mail.bcc);
		sockPuts(sfd,buf);
	}
	memset(buf,0,sizeof(buf));
	_snprintf(buf,sizeof(buf)-1,"X-Mailer: %s\r\n",MAILSEND_VERSION);
	sockPuts(sfd,buf);
	
	
	sockPuts(sfd,"\r\n");
	
	if ((Profile.bMailSplitConfig || (mail.options & MAIL_OPTION_MSG)) && szBody[0])
	{
		// FIX [SmtpDotStuff]: escape body lines starting with '.' per RFC 5321
		char szBodyDs[2 * MAX_MAIL_LEN + 64];
		smtpDotStuff(szBodyDs, sizeof(szBodyDs), szBody);
		sockPuts(sfd, szBodyDs);
		sockPuts(sfd,"\r\n");
	}
	nSMTPemails++;
	return (0);
}


// returns 0 on failure or empty queue, 1 on success
int xSendMail(THEMAIL *pMail)
{
	int 	rc ;
	char *pTmp ;
	extern int nSMTPsessions;

	if(nBufferdMailStart == nBufferdMailEnd)
	{
		return(0) ;
	}

	pTmp = szMailBuffer[nBufferdMailEnd] ;
	nBufferdMailEnd++ ;
	if(nBufferdMailEnd >= MAX_MAIL) {
		nBufferdMailEnd = 0 ;
	}

	if (pMail->from == (char *) NULL) {
		OUTPUTDEBUGMSG((("No From address specified")));
		AddResponse("xSendMail(): No From address specified\n");
		return (0);
	}
	if (pMail->smtp_server == (char *) NULL) {
		pMail->smtp_server= "127.0.0.1" ;
		OUTPUTDEBUGMSG((("No smtp_server specified using default : %s"), pMail->smtp_server)) ;
	}
	if (pMail->smtp_port == -1) {
		pMail->smtp_port=MAILSEND_SMTP_PORT;
		OUTPUTDEBUGMSG((("No smtp_port specified using default port %d"), pMail->smtp_port));
	}
	if (pTmp == (char *) NULL) {
		pTmp = MAILSEND_DEF_SUB;
		OUTPUTDEBUGMSG((("No subject specified using default subject %s"), pTmp));
	}
	// FIX [SmtpHelo]: geen helo-fallback meer hier — smtpHelo()/smtpBuildHeloArg() bepaalt het
	// EHLO-argument pas wanneer de socket verbonden is (configured domein, anders [lokaal-IP]).

	// Retry once: a stale persistent socket (server-side idle timeout) fails on the
	// first transaction attempt; the second attempt reconnects fresh and succeeds.
	for(int attempt = 0; attempt < 2; attempt++)
	{
		if(g_persistSocket == INVALID_SOCKET)
		{
			g_persistSocket = smtpConnect(pMail->smtp_server, pMail->smtp_port);
			if(g_persistSocket == INVALID_SOCKET)
				return(0);
			nSMTPsessions++;
			if(smtpHelo(g_persistSocket) || smtpLogin(g_persistSocket))
			{
				smtpDisconnect(g_persistSocket);
				g_persistSocket = INVALID_SOCKET;
				return(0);
			}
		}

		rc = 0;
		if(smtpMailFrom(g_persistSocket))        rc = -1;
		else if(smtpRcptTo(g_persistSocket))     rc = -1;
		else if(smtpData(g_persistSocket))       rc = -1;
		else if(smtpMail(g_persistSocket, pTmp)) rc = -1;
		else if(smtpEom(g_persistSocket))        rc = -1;

		if(rc == 0)
			return(1);

		smtpDisconnect(g_persistSocket);
		g_persistSocket = INVALID_SOCKET;
		if(attempt == 0)
			AddResponse("SMTP: send failed on existing connection — retrying with fresh connection") ;
	}

	AddResponse("SMTP: send failed after retry — message dropped") ;
	return(0);
}


DWORD WINAPI MailThreadFunc(LPVOID lpData)
{
	ULONGLONG dwLastActivityMs = GetTickCount64() ;

	OUTPUTDEBUGMSG((("MailThreadFunc()")));

	while(keepbusy) {
		if(nBufferdMailStart == nBufferdMailEnd) {
			// Queue empty — block until SendMail signals or 5 s safety timeout.
			WaitForSingleObject(hMailEvent, 5000) ;
			// Close idle connection before the server times it out (typically 5 min).
			// Next message will reconnect fresh — no stale socket, no dropped message.
			if(g_persistSocket != INVALID_SOCKET &&
			   GetTickCount64() - dwLastActivityMs > 3 * 60 * 1000u)
			{
				smtpDisconnect(g_persistSocket) ;
				g_persistSocket = INVALID_SOCKET ;
				AddResponse("SMTP: idle connection closed (3 min) — will reconnect on next message") ;
			}
		} else {
			dwLastActivityMs = GetTickCount64() ;
			if(!xSendMail((THEMAIL *) lpData)) {
				Sleep(1000) ;   // send failed — brief pause before retry
			}
		}
	}

	// Cleanly close the persistent SMTP connection on thread exit.
	if(g_persistSocket != INVALID_SOCKET) {
		smtpQuit(g_persistSocket) ;
		smtpDisconnect(g_persistSocket) ;
		g_persistSocket = INVALID_SOCKET ;
	}

	OUTPUTDEBUGMSG((("MailThreadFunc() 	ExitThread(0)\n")));
	ExitThread(0);
	return 0;
}

void StartMail(int nOptions)
{
	DWORD dummy = 0;

//	OUTPUTDEBUGMSG((("StartMail()")));
	if(nOptions & MAIL_OPTION_ENABLE)
	{
		if(MailThread != 0)
		{
			OUTPUTDEBUGMSG((("StartMail() MailThread != 0  Mail is already Started!")));
			return;
		}
		hMailEvent = CreateEvent(NULL, FALSE, FALSE, NULL) ;
		keepbusy = TRUE ;
		MailThread = CreateThread(0,0,MailThreadFunc,(LPVOID) &mail,0, &dummy);
		OUTPUTDEBUGMSG((("StartMail() CreateThread\n")));
	}
	else
	{
		if(MailThread == 0)
		{
			OUTPUTDEBUGMSG((("StartMail() MailThread == 0  Mail is already Stopped!")));
			return;
		}
		keepbusy = FALSE ;
		if(hMailEvent) SetEvent(hMailEvent) ;   // wake thread so it sees keepbusy==FALSE
		WaitForSingleObject(MailThread, 3000) ; // wait for clean exit (smtpQuit + TLS can be slow)
		CloseHandle(MailThread);
		MailThread = 0;
		if(hMailEvent) { CloseHandle(hMailEvent) ; hMailEvent = NULL ; }
		OUTPUTDEBUGMSG((("StartMail() CloseHandle(MailThread)\n")));
	}
}


// FIX [MailSplit]: append the fields enabled in 'mask' to dst (same format as legacy SendMail)
static void AppendMailFields(char *dst, size_t dstLen, int mask,
	char *sz1, char *sz2, char *sz3, char *sz4, char *sz5, char *sz6, char *sz7, char *szLabel)
{
	size_t len = strlen(dst) ;
	if(mask & MAIL_OPTION_ADDRESS) len += _snprintf_s(dst + len, dstLen - len, _TRUNCATE, "%s ", sz1) ;
	if(mask & MAIL_OPTION_TIME)    len += _snprintf_s(dst + len, dstLen - len, _TRUNCATE, "%s ", sz2) ;
	if(mask & MAIL_OPTION_DATE)    len += _snprintf_s(dst + len, dstLen - len, _TRUNCATE, "%s ", sz3) ;
	if(mask & MAIL_OPTION_MODE)    len += _snprintf_s(dst + len, dstLen - len, _TRUNCATE, "%s ", sz4) ;
	if(mask & MAIL_OPTION_TYPE)    len += _snprintf_s(dst + len, dstLen - len, _TRUNCATE, "%s ", sz5) ;
	if(mask & MAIL_OPTION_BITRATE) len += _snprintf_s(dst + len, dstLen - len, _TRUNCATE, "%s ", sz6) ;
	if(mask & MAIL_OPTION_MESSAGE) len += _snprintf_s(dst + len, dstLen - len, _TRUNCATE, "%s ", sz7) ;
	if(mask & MAIL_OPTION_LABEL)
	{
		// FIX [MailSplit]: only prefix the label with "- " when other fields precede it;
		// a lone label would otherwise start with a stray hyphen.
		if(len > 0)
			len += _snprintf_s(dst + len, dstLen - len, _TRUNCATE, "- %s ", szLabel) ;
		else
			len += _snprintf_s(dst + len, dstLen - len, _TRUNCATE, "%s ", szLabel) ;
	}
}

int SendMail(HWND hResponse, bool bMatch, bool bMonitor_only, int iSeparateSMTP, char *sz1, char *sz2, char *sz3, char *sz4, char *sz5, char *sz6, char *sz7, char *szLabel)
{
	// FIX [Geheugenbeheer]: szBuffer vergroot van 1024 naar MAX_STR_LEN+256 (5376 bytes)
	// en alle wsprintf-aanroepen vervangen door _snprintf_s met _TRUNCATE om stack-overflow
	// te voorkomen bij lange FLEX/POCSAG berichten (MSG_MESSAGE tot 5120 bytes).
	int	 len = 0 ;
	char szBuffer[MAX_STR_LEN + 256] = { 0 } ;
//	char szSubject[1024]="";

//	OUTPUTDEBUGMSG((("SendMail()")));
	mail.hResponse = hResponse ;
	if(hResponse) 
	{
		SendMessage(hResponse, LB_RESETCONTENT, 0, 0L) ;
	}
	if(mail.options & MAIL_OPTION_ENABLE) 
	{
		switch(mail.options & MAIL_OPTION_MODES) 
		{
			case MAIL_OPTION_MODE_ALL :
				OUTPUTDEBUGMSG((("SendMail() Send : Mode All")));
			break ;

			case MAIL_OPTION_MODE_FILTER :	
			if(!bMatch || bMonitor_only)
			{
				OUTPUTDEBUGMSG((("SendMail() Not Send : !bMatch || bMonitor_only")));
				return(0) ;
			}
			OUTPUTDEBUGMSG((("SendMail() Send : bMatch(%d) || bMonitor_only(%d)"), bMatch, bMonitor_only));
			break ;

			case MAIL_OPTION_MODE_MONITOR :
			if(!bMatch)
			{
				OUTPUTDEBUGMSG((("SendMail() Not Send: !bMatch")));
				return(0) ;
			}
			OUTPUTDEBUGMSG((("SendMail() Send : bMatch(%d) || bMonitor_only(%d)"), bMatch, bMonitor_only));

			break ;
			case MAIL_OPTION_MODE_SELECTABLE :
			if(!bMatch || !iSeparateSMTP)
			{
				OUTPUTDEBUGMSG((("SendMail() Not Send: !bMatch || !iSeparateSMTP")));
				return(0) ;
			}
			OUTPUTDEBUGMSG((("SendMail() Send : iSeparateSMTP(%d)"), iSeparateSMTP));
			break ;
		}
	}
	else 
	{
		OUTPUTDEBUGMSG((("SendMail() Mail Disabled")));
		return(0) ;
	}

	if(Profile.bMailSplitConfig)
	{
		// FIX [MailSplit]: build Subject and Body independently, joined by MAIL_SPLIT_SEP.
		char szSubj[MAX_STR_LEN + 128] = { 0 } ;
		char szBody[MAX_STR_LEN + 128] = { 0 } ;
		AppendMailFields(szSubj, sizeof(szSubj), Profile.nMailSubjectOptions,
			sz1, sz2, sz3, sz4, sz5, sz6, sz7, szLabel) ;
		AppendMailFields(szBody, sizeof(szBody), Profile.nMailBodyOptions,
			sz1, sz2, sz3, sz4, sz5, sz6, sz7, szLabel) ;

		if(szSubj[0] || szBody[0])
			len = _snprintf_s(szBuffer, sizeof(szBuffer), _TRUNCATE, "%s%c%s", szSubj, MAIL_SPLIT_SEP, szBody) ;
		else
			szBuffer[0] = '\0' ;	// nothing selected — don't queue an empty mail
	}
	else
	{
		// legacy behaviour: one concatenated blob, routed to Subject/Body via the SENDIN bits
		if(mail.options & MAIL_OPTION_ADDRESS)
		{
			len += _snprintf_s(szBuffer + len, sizeof(szBuffer) - len, _TRUNCATE, "%s ", sz1) ;
		}
		if(mail.options & MAIL_OPTION_TIME)
		{
			len += _snprintf_s(szBuffer + len, sizeof(szBuffer) - len, _TRUNCATE, "%s ", sz2) ;
		}
		if(mail.options & MAIL_OPTION_DATE)
		{
			len += _snprintf_s(szBuffer + len, sizeof(szBuffer) - len, _TRUNCATE, "%s ", sz3) ;
		}
		if(mail.options & MAIL_OPTION_MODE)
		{
			len += _snprintf_s(szBuffer + len, sizeof(szBuffer) - len, _TRUNCATE, "%s ", sz4) ;
		}
		if(mail.options & MAIL_OPTION_TYPE)
		{
			len += _snprintf_s(szBuffer + len, sizeof(szBuffer) - len, _TRUNCATE, "%s ", sz5) ;
		}
		if(mail.options & MAIL_OPTION_BITRATE)
		{
			len += _snprintf_s(szBuffer + len, sizeof(szBuffer) - len, _TRUNCATE, "%s ", sz6) ;
		}
		if(mail.options & MAIL_OPTION_MESSAGE)
		{
			len += _snprintf_s(szBuffer + len, sizeof(szBuffer) - len, _TRUNCATE, "%s ", sz7) ;
		}
		if(mail.options & MAIL_OPTION_LABEL)
		{
			len += _snprintf_s(szBuffer + len, sizeof(szBuffer) - len, _TRUNCATE, "- %s ", szLabel) ;
		}
	}

	if(!mail.smtp_port)
	{
		OUTPUTDEBUGMSG((("SendMail() Error: MailInit NOT called!")));
		nSMTPerrors++;		// PH: Counts # of Errors

		return(-1) ;
	}

	nMaxLen = 0 ;
	if(szBuffer[0])
	{
		OUTPUTDEBUGMSG((("SendMail() Send : >%s<\n"), szBuffer));
		strncpy(szMailBuffer[nBufferdMailStart], szBuffer, MAX_MAIL_LEN) ;
		nBufferdMailStart++ ;

		if(nBufferdMailStart >= MAX_MAIL)
		{
			nBufferdMailStart = 0 ;
		}
		if(hMailEvent) SetEvent(hMailEvent) ;   // wake mail thread immediately
	}
//	OUTPUTDEBUGMSG((("SendMail() nBufferdMailStart %d nBufferdMailEnd %d\n"), nBufferdMailStart, nBufferdMailEnd));
	return(0) ;
}

// FIX [SmtpLog]: convert SMTP error code to human-readable message + optionally log to file
extern char szPath[];
const char *GetSmtpErrorMessage(int errCode)
{
	static char szBuf[256];
	const char *msg = "Unknown error";

	switch (errCode) {
		case 0: return ""; // no error
		case 100: msg = "Winsock initialization failed"; break;
		case 101: msg = "Winsock version error"; break;
		case 102: msg = "Socket send error"; break;
		case 103: msg = "Socket receive error"; break;
		case 104: msg = "Cannot connect to SMTP server"; break;
		case 105: msg = "Cannot resolve server hostname"; break;
		case 106: msg = "Invalid socket"; break;
		case 107: msg = "Invalid hostname"; break;
		case 108: msg = "Socket configuration error"; break;
		case 109: msg = "Connection timeout (select)"; break;
		case 110: msg = "Invalid IPv4 address"; break;
		case 200: msg = "Message header not defined"; break;
		case 201: msg = "From address not defined"; break;
		case 202: msg = "Subject not defined"; break;
		case 203: msg = "No recipients defined"; break;
		case 204: msg = "Login not defined"; break;
		case 205: msg = "Password not defined"; break;
		case 206: msg = "Incorrect username/password"; break;
		case 207: msg = "DIGEST authentication failed"; break;
		case 208: msg = "Invalid server name"; break;
		case 209: msg = "No recipient email"; break;
		case 300: msg = "Server rejected MAIL FROM"; break;
		case 301: msg = "Server rejected EHLO"; break;
		case 302: msg = "PLAIN authentication not supported"; break;
		case 303: msg = "LOGIN authentication not supported"; break;
		case 304: msg = "CRAM-MD5 not supported"; break;
		case 305: msg = "DIGEST-MD5 not supported"; break;
		case 306: msg = "DIGEST-MD5 error"; break;
		case 307: msg = "Server rejected DATA command"; break;
		case 308: msg = "Error sending QUIT"; break;
		case 309: msg = "Server rejected RCPT TO (invalid recipient)"; break;
		case 310: msg = "Message body error"; break;
		case 400: msg = "Server closed connection unexpectedly"; break;
		case 401: msg = "Server not ready (check credentials/firewall)"; break;
		case 402: msg = "SMTP server not responding"; break;
		case 403: msg = "Connection timeout"; break;
		case 404: msg = "Attachment file not found"; break;
		case 405: msg = "Message too large"; break;
		case 406: msg = "Invalid login credentials"; break;
		case 407: msg = "Unexpected server response"; break;
		case 408: msg = "Out of memory"; break;
		case 409: msg = "Time error"; break;
		case 410: msg = "Receive buffer empty"; break;
		case 411: msg = "Send buffer empty"; break;
		case 412: msg = "Message queue overflow"; break;
		case 413: msg = "Server STARTTLS error"; break;
		case 414: msg = "SSL/TLS initialization error"; break;
		case 415: msg = "DATA block error"; break;
		case 416: msg = "Server does not support STARTTLS"; break;
		case 417: msg = "Server does not support LOGIN auth"; break;
		case 553: msg = "Recipient address rejected (Gmail may reject forwarding addresses)"; break;
	}

	_snprintf(szBuf, sizeof(szBuf) - 1, "Error %d: %s", errCode, msg);
	szBuf[sizeof(szBuf) - 1] = '\0';
	return szBuf;
}

void LogSmtpError(int errCode)
{
	if (errCode == 0) return;

	const char *msg = GetSmtpErrorMessage(errCode);

	// Store in global for display in Setup dialog (user-readable, always)
	_snprintf(g_szLastSmtpError, sizeof(g_szLastSmtpError) - 1, "%s", msg);
	g_szLastSmtpError[sizeof(g_szLastSmtpError) - 1] = '\0';

	// FIX [SmtpLog]: send to Monitor window (IDC_SMTP_RESPONSE)
	char szBuf[512];
	_snprintf(szBuf, sizeof(szBuf) - 1, "SMTP Error: %s\n", msg);
	szBuf[sizeof(szBuf) - 1] = '\0';
	AddResponse(szBuf);

	// Optionally log to disk if checkbox enabled
	if (!Profile.bMailLogErrors) return;

	char szLog[MAX_PATH];
	const char *root = (Profile.LogfilePath[0]) ? Profile.LogfilePath : szPath;
	if (!root || !root[0]) return;

	_snprintf(szLog, sizeof(szLog) - 1, "%s\\pdw_smtp_error.log", root);
	szLog[sizeof(szLog) - 1] = '\0';

	FILE *f = fopen(szLog, "a");
	if (f) {
		SYSTEMTIME st;
		GetLocalTime(&st);
		fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d | %s\n",
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, msg);
		fclose(f);
	}
}

// FIX [SmtpLog]: getter for last SMTP error (for Setup dialog)
const char *GetLastSmtpError(void)
{
	return g_szLastSmtpError;
}

int MailInit(char *szMailHost, char *szMailHeloDomain, char *szMailFrom, char *szMailTo, char *szMailUser, char *szMailPassword, int iMailPort, int nOptions)
{
	StartMail(0) ;              // stop existing thread cleanly before reconfiguring (idempotent if not running)
	memset(&mail, 0, sizeof(mail)) ;
	mail.from = szMailFrom ;
	mail.to = szMailTo ;
	mail.cc = NULL ;
	mail.bcc = NULL ;
	mail.smtp_server = szMailHost ;
	mail.helo_domain = szMailHeloDomain ;
	mail.user = szMailUser ;
	mail.password = szMailPassword ;
	mail.smtp_port = iMailPort ;
	mail.options = nOptions ;
	StartMail(nOptions) ;
	return(0) ;
}
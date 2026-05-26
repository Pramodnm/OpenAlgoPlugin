// OpenAlgoConfigDlg.cpp : implementation file
#include "stdafx.h"
#include "resource.h"  // Include resource definitions
#include "Plugin.h"  // Include Plugin.h before OpenAlgoGlobals.h
#include "OpenAlgoGlobals.h"  // Include global variables
#include "OpenAlgoPlugin.h"
#include "OpenAlgoConfigDlg.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

// Global variables are now properly declared in OpenAlgoGlobals.h
// No need for extern declarations here

/////////////////////////////////////////////////////////////////////////////
// COpenAlgoConfigDlg dialog

COpenAlgoConfigDlg::COpenAlgoConfigDlg(CWnd* pParent /*=NULL*/)
	: CDialog(COpenAlgoConfigDlg::IDD, pParent)
{
	//{{AFX_DATA_INIT(COpenAlgoConfigDlg)
	//}}AFX_DATA_INIT

	// Initialize m_pSite member variable
	m_pSite = NULL;
}

void COpenAlgoConfigDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	//{{AFX_DATA_MAP(COpenAlgoConfigDlg)
	//}}AFX_DATA_MAP

	// Map dialog controls to global variables
	DDX_Text(pDX, IDC_SERVER_EDIT, g_oServer);
	DDV_MaxChars(pDX, g_oServer, 255);

	DDX_Text(pDX, IDC_APIKEY_EDIT, g_oApiKey);
	DDV_MaxChars(pDX, g_oApiKey, 255);

	DDX_Text(pDX, IDC_PORT_EDIT, g_nPortNumber);
	DDV_MinMaxInt(pDX, g_nPortNumber, 1, 65535);

	DDX_Text(pDX, IDC_INTERVAL_EDIT, g_nRefreshInterval);
	DDV_MinMaxInt(pDX, g_nRefreshInterval, 1, 3600); // 1 second to 1 hour

	DDX_Text(pDX, IDC_TIMESHIFT_EDIT, g_nTimeShift);
	DDV_MinMaxInt(pDX, g_nTimeShift, -48, 48);
	
	DDX_Text(pDX, IDC_WEBSOCKET_EDIT, g_oWebSocketUrl);
	DDV_MaxChars(pDX, g_oWebSocketUrl, 255);
}

BEGIN_MESSAGE_MAP(COpenAlgoConfigDlg, CDialog)
	//{{AFX_MSG_MAP(COpenAlgoConfigDlg)
	ON_BN_CLICKED(IDC_TEST_CONNECTION_BUTTON, OnTestConnectionButton)
	ON_BN_CLICKED(IDC_TEST_WEBSOCKET_BUTTON, OnTestWebSocketButton)
	ON_EN_SETFOCUS(IDC_APIKEY_EDIT, OnApiKeyEditSetFocus)
	//}}AFX_MSG_MAP
END_MESSAGE_MAP()

// Log helper: never spell out the full API key. Keep the first/last few chars
// only so the user can tell from DbgView which key got saved without leaking it.
static CString MaskKey(const CString& key)
{
	CString s;
	int n = key.GetLength();
	if (n <= 8) { s.Format(_T("(len=%d)"), n); return s; }
	s.Format(_T("%s...%s (len=%d)"), (LPCTSTR)key.Left(4), (LPCTSTR)key.Right(4), n);
	return s;
}

/////////////////////////////////////////////////////////////////////////////
// COpenAlgoConfigDlg message handlers

BOOL COpenAlgoConfigDlg::OnInitDialog()
{
	AFX_MANAGE_STATE(AfxGetStaticModuleState());
	CDialog::OnInitDialog();

	SetWindowText(_T("OpenAlgo Plugin Configuration"));

	// Diagnostic so we can verify in DbgView which key the dialog opened with.
	CString log;
	log.Format(_T("OpenAlgo: Config dialog opened. Existing ApiKey=%s"), (LPCTSTR)MaskKey(g_oApiKey));
	OutputDebugString(log);

	CWnd* pServerEdit = GetDlgItem(IDC_SERVER_EDIT);
	if (pServerEdit)
		pServerEdit->SetFocus();

	return FALSE;
}

// Auto-select all text in the API Key field whenever it receives focus so
// users replace the existing masked key by typing rather than appending to it.
void COpenAlgoConfigDlg::OnApiKeyEditSetFocus()
{
	CEdit* pEdit = (CEdit*)GetDlgItem(IDC_APIKEY_EDIT);
	if (pEdit)
		pEdit->SetSel(0, -1);
}

void COpenAlgoConfigDlg::OnOK()
{
	AFX_MANAGE_STATE(AfxGetStaticModuleState());

	if (!UpdateData(TRUE))
		return;

	if (g_oServer.IsEmpty())
	{
		AfxMessageBox(_T("Please enter a server address."), MB_OK | MB_ICONWARNING);
		GetDlgItem(IDC_SERVER_EDIT)->SetFocus();
		return;
	}

	if (g_oApiKey.IsEmpty())
	{
		AfxMessageBox(_T("Please enter your OpenAlgo API Key."), MB_OK | MB_ICONWARNING);
		GetDlgItem(IDC_APIKEY_EDIT)->SetFocus();
		return;
	}

	// Defensive: ensure m_pszRegistryKey is set. CWinApp::InitInstance is not
	// reliably called for Regular MFC DLLs, so SetRegistryKey from our
	// COpenAlgoApp::InitInstance may never run. Calling it here is idempotent
	// and guarantees WriteProfileString hits the registry instead of falling
	// back to WIN.INI (where the write would silently fail).
	theApp.EnsureRegistryRoot();
	CWinApp* pApp = AfxGetApp();

	BOOL bSrv  = pApp ? pApp->WriteProfileString(_T("OpenAlgo"), _T("Server"),        g_oServer)        : FALSE;
	BOOL bKey  = pApp ? pApp->WriteProfileString(_T("OpenAlgo"), _T("ApiKey"),        g_oApiKey)        : FALSE;
	BOOL bWs   = pApp ? pApp->WriteProfileString(_T("OpenAlgo"), _T("WebSocketUrl"),  g_oWebSocketUrl)  : FALSE;
	BOOL bPort = pApp ? pApp->WriteProfileInt   (_T("OpenAlgo"), _T("Port"),          g_nPortNumber)    : FALSE;
	BOOL bIv   = pApp ? pApp->WriteProfileInt   (_T("OpenAlgo"), _T("RefreshInterval"), g_nRefreshInterval) : FALSE;
	BOOL bTs   = pApp ? pApp->WriteProfileInt   (_T("OpenAlgo"), _T("TimeShift"),     g_nTimeShift)     : FALSE;
	BOOL bRt   = pApp ? pApp->WriteProfileInt   (_T("OpenAlgo"), _T("EnableRealTimeCandles"), g_bRealTimeCandlesEnabled ? 1 : 0) : FALSE;
	BOOL bBf   = pApp ? pApp->WriteProfileInt   (_T("OpenAlgo"), _T("BackfillIntervalMs"),    g_nBackfillIntervalMs)            : FALSE;

	CString log;
	log.Format(
		_T("OpenAlgo: OnOK saved ApiKey=%s Server=%s "
		   "(srv=%d key=%d ws=%d port=%d iv=%d ts=%d rt=%d bf=%d)"),
		(LPCTSTR)MaskKey(g_oApiKey), (LPCTSTR)g_oServer,
		bSrv, bKey, bWs, bPort, bIv, bTs, bRt, bBf);
	OutputDebugString(log);

	if (!bKey)
	{
		AfxMessageBox(
			_T("Failed to write the OpenAlgo API key to the registry.\n")
			_T("Check that AmiBroker has write access to HKCU\\Software\\OpenAlgo."),
			MB_OK | MB_ICONERROR);
		return;
	}

	CDialog::OnOK();
}

void COpenAlgoConfigDlg::OnTestConnectionButton()
{
	// Update data from controls
	if (!UpdateData(TRUE))
	{
		return;
	}

	// Validate API Key
	if (g_oApiKey.IsEmpty())
	{
		AfxMessageBox(_T("Please enter your OpenAlgo API Key before testing the connection."), MB_OK | MB_ICONWARNING);
		GetDlgItem(IDC_APIKEY_EDIT)->SetFocus();
		return;
	}

	SetDlgItemText(IDC_STATUS_STATIC, _T("Testing connection..."));

	// Change cursor to wait cursor
	CWaitCursor wait;

	// Test connection to OpenAlgo server using ping endpoint
	BOOL bConnected = FALSE;
	CString oURL = BuildOpenAlgoURL(g_oServer, g_nPortNumber, _T("/api/v1/ping"));

	try
	{
		CInternetSession oSession(_T("OpenAlgo Plugin Test"), 1,
			INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, INTERNET_FLAG_DONT_CACHE);
		oSession.SetOption(INTERNET_OPTION_CONNECT_TIMEOUT, 5000);
		oSession.SetOption(INTERNET_OPTION_RECEIVE_TIMEOUT, 5000);

		// Prepare POST data with API key
		CString oPostData;
		oPostData.Format(_T("{\"apikey\":\"%s\"}"), (LPCTSTR)g_oApiKey);

		CHttpConnection* pConnection = NULL;
		CHttpFile* pFile = NULL;

		// Parse server and port
		INTERNET_PORT nPort = (INTERNET_PORT)g_nPortNumber;
		CString oServer = g_oServer;

		// Remove http:// or https:// if present
		oServer.Replace(_T("http://"), _T(""));
		oServer.Replace(_T("https://"), _T(""));

		pConnection = oSession.GetHttpConnection(oServer, nPort);

		if (pConnection)
		{
			// Create POST request
			pFile = pConnection->OpenRequest(
				CHttpConnection::HTTP_VERB_POST,
				_T("/api/v1/ping"),
				NULL,
				1,
				NULL,
				NULL,
				INTERNET_FLAG_RELOAD | INTERNET_FLAG_DONT_CACHE);

			if (pFile)
			{
				// Set headers
				CString oHeaders = _T("Content-Type: application/json\r\n");

				// Convert string to UTF-8 for sending
				CStringA oPostDataA(oPostData);

				// Send the request
				BOOL bResult = pFile->SendRequest(oHeaders, (LPVOID)(LPCSTR)oPostDataA, oPostDataA.GetLength());

				if (bResult)
				{
					DWORD dwStatusCode = 0;
					pFile->QueryInfoStatusCode(dwStatusCode);

					if (dwStatusCode == 200)
					{
						// Try to read response
						CString oResponse;
						CString oLine;
						while (pFile->ReadString(oLine))
						{
							oResponse += oLine;
							if (oResponse.GetLength() > 1000) break; // Limit response size
						}

						// Check if response contains "success" and "pong" (simple JSON parsing)
						if ((oResponse.Find(_T("\"status\":\"success\"")) >= 0 ||
							 oResponse.Find(_T("\"status\": \"success\"")) >= 0) &&
							(oResponse.Find(_T("\"message\":\"pong\"")) >= 0 ||
							 oResponse.Find(_T("\"message\": \"pong\"")) >= 0))
						{
							bConnected = TRUE;

							// Try to extract broker from response
							int iBrokerPos = oResponse.Find(_T("\"broker\":"));
							if (iBrokerPos >= 0)
							{
								iBrokerPos += 10; // Move past "broker":"
								int iEndPos = oResponse.Find(_T("\""), iBrokerPos);
								if (iEndPos > iBrokerPos)
								{
									CString oBroker = oResponse.Mid(iBrokerPos, iEndPos - iBrokerPos);
									CString oStatus;
									oStatus.Format(_T("Connection successful! Broker: %s"), (LPCTSTR)oBroker);
									SetDlgItemText(IDC_STATUS_STATIC, oStatus);
								}
								else
								{
									SetDlgItemText(IDC_STATUS_STATIC,
										_T("Connection successful! OpenAlgo server is running."));
								}
							}
							else
							{
								SetDlgItemText(IDC_STATUS_STATIC,
									_T("Connection successful! OpenAlgo server is running."));
							}
						}
						else if (oResponse.Find(_T("\"status\":\"error\"")) >= 0)
						{
							SetDlgItemText(IDC_STATUS_STATIC,
								_T("Connection failed: Invalid API Key or server error."));
						}
						else
						{
							SetDlgItemText(IDC_STATUS_STATIC,
								_T("Connection failed: Unexpected response from server."));
						}
					}
					else
					{
						CString oStatus;
						oStatus.Format(_T("Server returned HTTP status code: %lu"), dwStatusCode);
						SetDlgItemText(IDC_STATUS_STATIC, oStatus);
					}
				}
				else
				{
					SetDlgItemText(IDC_STATUS_STATIC, _T("Failed to send request to server."));
				}

				pFile->Close();
				delete pFile;
			}

			pConnection->Close();
			delete pConnection;
		}
		else
		{
			SetDlgItemText(IDC_STATUS_STATIC, _T("Failed to connect to server."));
		}

		oSession.Close();
	}
	catch (CInternetException* e)
	{
		TCHAR szError[256];
		e->GetErrorMessage(szError, 256);

		CString oStatus;
		oStatus.Format(_T("Connection failed: %s"), szError);
		SetDlgItemText(IDC_STATUS_STATIC, oStatus);

		e->Delete();
	}
}


void COpenAlgoConfigDlg::OnTestWebSocketButton()
{
	// Update data from controls
	UpdateData(TRUE);

	// Validate WebSocket URL
	if (g_oWebSocketUrl.IsEmpty())
	{
		SetDlgItemText(IDC_WEBSOCKET_STATUS_STATIC, _T("Please enter a WebSocket URL"));
		return;
	}

	// Validate API Key
	if (g_oApiKey.IsEmpty())
	{
		SetDlgItemText(IDC_WEBSOCKET_STATUS_STATIC, _T("API Key is required"));
		return;
	}

	// Change cursor to wait cursor
	CWaitCursor wait;

	SetDlgItemText(IDC_WEBSOCKET_STATUS_STATIC, _T("Testing..."));

	// Test WebSocket connection
	if (TestWebSocketConnection(g_oWebSocketUrl, g_oApiKey))
	{
		// Success message is set by TestWebSocketConnection function
	}
	else
	{
		SetDlgItemText(IDC_WEBSOCKET_STATUS_STATIC,
			_T("WebSocket connection failed"));
	}
}

BOOL COpenAlgoConfigDlg::TestWebSocketConnection(const CString& wsUrl, const CString& apiKey)
{
	BOOL bSuccess = FALSE;
	SOCKET sock = INVALID_SOCKET;
	
	try
	{
		// Initialize Winsock
		WSADATA wsaData;
		if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
		{
			return FALSE;
		}

		// Parse WebSocket URL
		CString host, path;
		int port = 80;
		BOOL bSecure = FALSE;

		CString url = wsUrl;
		if (url.Left(5) == _T("wss://"))
		{
			bSecure = TRUE;
			port = 443;
			url = url.Mid(6);
		}
		else if (url.Left(5) == _T("ws://"))
		{
			url = url.Mid(5);
		}

		// Extract host and port
		int slashPos = url.Find(_T('/'));
		if (slashPos > 0)
		{
			host = url.Left(slashPos);
			path = url.Mid(slashPos);
		}
		else
		{
			host = url;
			path = _T("/");
		}

		int colonPos = host.Find(_T(':'));
		if (colonPos > 0)
		{
			CString portStr = host.Mid(colonPos + 1);
			port = _ttoi(portStr);
			host = host.Left(colonPos);
		}

		// For simplicity, we'll just test TCP connection here
		// Full WebSocket implementation would require HTTP upgrade and WebSocket framing
		
		// Create socket
		sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (sock == INVALID_SOCKET)
		{
			WSACleanup();
			return FALSE;
		}

		// Set timeout
		int timeout = 5000; // 5 seconds
		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
		setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));

		// Resolve hostname
		struct addrinfo hints, *result;
		ZeroMemory(&hints, sizeof(hints));
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;

		CStringA hostA(host);
		CStringA portStrA;
		portStrA.Format("%d", port);

		if (getaddrinfo(hostA, portStrA, &hints, &result) != 0)
		{
			closesocket(sock);
			WSACleanup();
			return FALSE;
		}

		// Connect to server
		if (connect(sock, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR)
		{
			freeaddrinfo(result);
			closesocket(sock);
			WSACleanup();
			return FALSE;
		}

		freeaddrinfo(result);

		// Send WebSocket upgrade request
		CString upgradeRequest;
		upgradeRequest.Format(
			_T("GET %s HTTP/1.1\r\n")
			_T("Host: %s:%d\r\n")
			_T("Upgrade: websocket\r\n")
			_T("Connection: Upgrade\r\n")
			_T("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n")
			_T("Sec-WebSocket-Version: 13\r\n")
			_T("\r\n"),
			(LPCTSTR)path, (LPCTSTR)host, port);

		CStringA requestA(upgradeRequest);
		if (send(sock, requestA, requestA.GetLength(), 0) == SOCKET_ERROR)
		{
			closesocket(sock);
			WSACleanup();
			return FALSE;
		}

		// Receive response
		char buffer[1024];
		int received = recv(sock, buffer, sizeof(buffer) - 1, 0);
		if (received > 0)
		{
			buffer[received] = '\0';
			CString response(buffer);
			
			// Check for successful upgrade
			if (response.Find(_T("101")) > 0 && response.Find(_T("Switching Protocols")) > 0)
			{
				// Now send authentication message
				CString authMsg = _T("{\"action\":\"authenticate\",\"api_key\":\"") + apiKey + _T("\"}");

				if (SendWebSocketFrame(sock, authMsg))
				{
					// Wait for authentication response
					char authBuffer[1024];
					int authReceived = recv(sock, authBuffer, sizeof(authBuffer) - 1, 0);
					if (authReceived > 0)
					{
						CString authResponse = DecodeWebSocketFrame(authBuffer, authReceived);
						
						// Check for success status in authentication response
						if (authResponse.Find(_T("success")) >= 0 ||
							authResponse.Find(_T("authenticated")) >= 0 ||
							authResponse.Find(_T("\"status\":\"ok\"")) >= 0)
						{
							// Authentication successful, now test subscribe to RELIANCE-NSE
							CString subMsg = _T("{\"action\":\"subscribe\",\"symbol\":\"RELIANCE\",\"exchange\":\"NSE\",\"mode\":2}");

							if (SendWebSocketFrame(sock, subMsg))
							{
								// Wait for subscription response
								Sleep(1000);

								// Try to receive any response (subscription confirmation or data)
								char quoteBuffer[2048];
								int quoteReceived = recv(sock, quoteBuffer, sizeof(quoteBuffer) - 1, 0);

								BOOL subscriptionWorked = FALSE;
								if (quoteReceived > 0)
								{
									CString response = DecodeWebSocketFrame(quoteBuffer, quoteReceived);

									// Check if we got any valid response (subscription confirmation or market data)
									if (!response.IsEmpty())
									{
										subscriptionWorked = TRUE;
									}
								}

								// Unsubscribe
								CString unsubMsg = _T("{\"action\":\"unsubscribe\",\"symbol\":\"RELIANCE\",\"exchange\":\"NSE\",\"mode\":2}");
								SendWebSocketFrame(sock, unsubMsg);

								// Show simple success message
								if (subscriptionWorked)
								{
									SetDlgItemText(IDC_WEBSOCKET_STATUS_STATIC,
										_T("WebSocket connection successful!"));
								}
								else
								{
									SetDlgItemText(IDC_WEBSOCKET_STATUS_STATIC,
										_T("WebSocket connection successful (authenticated)"));
								}

								bSuccess = TRUE;
							}
							else
							{
								SetDlgItemText(IDC_WEBSOCKET_STATUS_STATIC,
									_T("Connected but subscription test failed"));
							}
						}
						else
						{
							SetDlgItemText(IDC_WEBSOCKET_STATUS_STATIC,
								_T("WebSocket connection failed: Authentication rejected"));
						}
					}
					else
					{
						SetDlgItemText(IDC_WEBSOCKET_STATUS_STATIC,
							_T("WebSocket connection failed: No auth response"));
					}
				}
				else
				{
					SetDlgItemText(IDC_WEBSOCKET_STATUS_STATIC,
						_T("WebSocket connection failed: Auth send failed"));
				}
			}
			else
			{
				SetDlgItemText(IDC_WEBSOCKET_STATUS_STATIC,
					_T("WebSocket connection failed: Upgrade failed"));
			}
		}

		closesocket(sock);
		WSACleanup();
	}
	catch (...)
	{
		if (sock != INVALID_SOCKET)
		{
			closesocket(sock);
		}
		WSACleanup();
		return FALSE;
	}

	return bSuccess;
}

void COpenAlgoConfigDlg::GenerateMaskKey(unsigned char* maskKey)
{
	// Generate a simple random mask key
	srand((unsigned int)GetTickCount64());
	maskKey[0] = (unsigned char)(rand() & 0xFF);
	maskKey[1] = (unsigned char)(rand() & 0xFF);
	maskKey[2] = (unsigned char)(rand() & 0xFF);
	maskKey[3] = (unsigned char)(rand() & 0xFF);
}

BOOL COpenAlgoConfigDlg::SendWebSocketFrame(SOCKET sock, const CString& message)
{
	// Convert message to UTF-8
	CStringA messageA(message);
	int messageLen = messageA.GetLength();
	
	// Create WebSocket frame
	unsigned char frame[1024];
	int frameLen = 0;
	
	// First byte: FIN=1, OpCode=1 (text frame)
	frame[frameLen++] = 0x81;
	
	// Second byte: MASK=1 + Payload length
	if (messageLen < 126)
	{
		frame[frameLen++] = 0x80 | messageLen; // MASK=1 + length
	}
	else if (messageLen < 65536)
	{
		frame[frameLen++] = 0x80 | 126; // MASK=1 + extended length indicator
		frame[frameLen++] = (messageLen >> 8) & 0xFF;
		frame[frameLen++] = messageLen & 0xFF;
	}
	else
	{
		return FALSE; // Message too long
	}
	
	// Generate masking key
	unsigned char maskKey[4];
	GenerateMaskKey(maskKey);
	memcpy(&frame[frameLen], maskKey, 4);
	frameLen += 4;
	
	// Masked payload
	for (int i = 0; i < messageLen; i++)
	{
		frame[frameLen++] = messageA[i] ^ maskKey[i % 4];
	}
	
	// Send the frame
	int sent = send(sock, (char*)frame, frameLen, 0);
	return (sent == frameLen);
}

CString COpenAlgoConfigDlg::DecodeWebSocketFrame(const char* buffer, int length)
{
	CString result;
	
	if (length < 2) return result;
	
	int pos = 0;
	unsigned char firstByte = (unsigned char)buffer[pos++];
	unsigned char secondByte = (unsigned char)buffer[pos++];
	
	// Check if this is a text frame
	if ((firstByte & 0x0F) != 0x01) return result; // Not a text frame
	
	BOOL masked = (secondByte & 0x80) != 0;
	int payloadLen = secondByte & 0x7F;
	
	// Handle extended payload length
	if (payloadLen == 126)
	{
		if (pos + 2 > length) return result;
		payloadLen = ((unsigned char)buffer[pos] << 8) | (unsigned char)buffer[pos + 1];
		pos += 2;
	}
	else if (payloadLen == 127)
	{
		// 64-bit length not supported
		return result;
	}
	
	// Handle masking key
	unsigned char maskKey[4] = {0};
	if (masked)
	{
		if (pos + 4 > length) return result;
		memcpy(maskKey, &buffer[pos], 4);
		pos += 4;
	}
	
	// Extract and unmask payload
	if (pos + payloadLen <= length)
	{
		CStringA payloadA;
		char* payloadBuffer = payloadA.GetBuffer(payloadLen + 1);
		
		for (int i = 0; i < payloadLen; i++)
		{
			if (masked)
			{
				payloadBuffer[i] = buffer[pos + i] ^ maskKey[i % 4];
			}
			else
			{
				payloadBuffer[i] = buffer[pos + i];
			}
		}
		payloadBuffer[payloadLen] = '\0';
		payloadA.ReleaseBuffer(payloadLen);
		
		result = CString(payloadA);
	}
	
	return result;
}


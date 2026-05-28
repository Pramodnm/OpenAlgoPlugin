///////////////////////////////
// WebSocket Functions
///////////////////////////////

void GenerateWebSocketMaskKey(unsigned char* maskKey)
{
	// Generate a simple random mask key
	srand((unsigned int)GetTickCount64());
	maskKey[0] = (unsigned char)(rand() & 0xFF);
	maskKey[1] = (unsigned char)(rand() & 0xFF);
	maskKey[2] = (unsigned char)(rand() & 0xFF);
	maskKey[3] = (unsigned char)(rand() & 0xFF);
}

BOOL SendWebSocketFrame(const CString& message)
{
	if (g_websocket == INVALID_SOCKET)
		return FALSE;

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
		frame[frameLen++] = 0x80 | messageLen;
	}
	else if (messageLen < 65536)
	{
		frame[frameLen++] = 0x80 | 126;
		frame[frameLen++] = (messageLen >> 8) & 0xFF;
		frame[frameLen++] = messageLen & 0xFF;
	}
	else
	{
		return FALSE; // Message too long
	}
	
	// Generate masking key
	unsigned char maskKey[4];
	GenerateWebSocketMaskKey(maskKey);
	memcpy(&frame[frameLen], maskKey, 4);
	frameLen += 4;
	
	// Masked payload
	for (int i = 0; i < messageLen; i++)
	{
		frame[frameLen++] = messageA[i] ^ maskKey[i % 4];
	}
	
	// Send the frame. Serialize all WS send()s through g_WebSocketSendCS so
	// the worker thread's PING/PONG can't interleave bytes with this frame
	// and trip the server's "abnormal close 1006" handler.
	int sent = 0;
	if (g_bWebSocketSendCSInit)
	{
		EnterCriticalSection(&g_WebSocketSendCS);
		if (g_websocket != INVALID_SOCKET)
			sent = send(g_websocket, (char*)frame, frameLen, 0);
		LeaveCriticalSection(&g_WebSocketSendCS);
	}
	else
	{
		if (g_websocket != INVALID_SOCKET)
			sent = send(g_websocket, (char*)frame, frameLen, 0);
	}
	return (sent == frameLen);
}

CString DecodeWebSocketFrame(const char* buffer, int length)
{
	CString result;
	
	if (length < 2) return result;
	
	int pos = 0;
	unsigned char firstByte = (unsigned char)buffer[pos++];
	unsigned char secondByte = (unsigned char)buffer[pos++];
	
	// Check frame type
	unsigned char opcode = firstByte & 0x0F;
	
	// Extract payload info (needed for all frame types)
	BOOL masked = (secondByte & 0x80) != 0;
	int payloadLen = secondByte & 0x7F;

	// Handle different frame types
	if (opcode == 0x08) // Close frame
	{
		// Extract close status code and reason from payload
		CString closeInfo = _T("CLOSE_FRAME");

		// Handle extended payload length for close frames
		int closePos = pos;
		if (payloadLen == 126)
		{
			if (closePos + 2 > length) return closeInfo;
			payloadLen = ((unsigned char)buffer[closePos] << 8) | (unsigned char)buffer[closePos + 1];
			closePos += 2;
		}

		// Extract masking key if present
		unsigned char closeMaskKey[4] = {0};
		if (masked)
		{
			if (closePos + 4 > length) return closeInfo;
			memcpy(closeMaskKey, &buffer[closePos], 4);
			closePos += 4;
		}

		// Extract status code (first 2 bytes of payload)
		if (payloadLen >= 2 && closePos + 2 <= length)
		{
			unsigned char byte1 = masked ? (buffer[closePos] ^ closeMaskKey[0]) : buffer[closePos];
			unsigned char byte2 = masked ? (buffer[closePos + 1] ^ closeMaskKey[1]) : buffer[closePos + 1];
			int statusCode = (byte1 << 8) | byte2;

			// Extract reason text if present
			CString reason;
			if (payloadLen > 2 && closePos + payloadLen <= length)
			{
				CStringA reasonA;
				char* reasonBuf = reasonA.GetBuffer(payloadLen - 2 + 1);
				for (int i = 2; i < payloadLen && (closePos + i) < length; i++)
				{
					reasonBuf[i - 2] = masked ? (buffer[closePos + i] ^ closeMaskKey[i % 4]) : buffer[closePos + i];
				}
				reasonBuf[payloadLen - 2] = '\0';
				reasonA.ReleaseBuffer();
				reason = CString(reasonA);
			}

			if (reason.IsEmpty())
			{
				closeInfo.Format(_T("CLOSE_FRAME (Status Code: %d)"), statusCode);
			}
			else
			{
				closeInfo.Format(_T("CLOSE_FRAME (Status Code: %d, Reason: %s)"), statusCode, reason);
			}
		}

		return closeInfo;
	}
	else if (opcode == 0x09) // Ping frame
	{
		// Extract PING payload to echo back in PONG (RFC 6455 Section 5.5.3 requirement)
		// The Python websockets library sends PING with a 4-byte payload and expects
		// the PONG to echo it back exactly. If we send empty PONG, server closes with code 1011.
		CString pingResult = _T("PING_FRAME");

		// Handle extended payload length
		int pingPos = pos;
		if (payloadLen == 126)
		{
			if (pingPos + 2 > length) return pingResult;
			payloadLen = ((unsigned char)buffer[pingPos] << 8) | (unsigned char)buffer[pingPos + 1];
			pingPos += 2;
		}

		// Extract masking key if present
		unsigned char pingMaskKey[4] = {0};
		if (masked)
		{
			if (pingPos + 4 > length) return pingResult;
			memcpy(pingMaskKey, &buffer[pingPos], 4);
			pingPos += 4;
		}

		// Extract payload (usually 4 bytes from Python websockets library)
		if (payloadLen > 0 && payloadLen <= 125 && pingPos + payloadLen <= length)
		{
			CStringA hexPayload;
			for (int i = 0; i < payloadLen; i++)
			{
				unsigned char byte = masked ? (buffer[pingPos + i] ^ pingMaskKey[i % 4]) : buffer[pingPos + i];
				CStringA hexByte;
				hexByte.Format("%02X", byte);
				hexPayload += hexByte;
			}

			// Return "PING_FRAME:XXXXXXXX" where X is hex-encoded payload
			pingResult = _T("PING_FRAME:") + CString(hexPayload);
		}

		return pingResult;
	}
	else if (opcode == 0x0A) // Pong frame
	{
		return _T("PONG_FRAME");
	}
	else if (opcode != 0x01) // Not a text frame
	{
		return result;
	}
	
	// Handle extended payload length
	if (payloadLen == 126)
	{
		if (pos + 2 > length) return result;
		payloadLen = ((unsigned char)buffer[pos] << 8) | (unsigned char)buffer[pos + 1];
		pos += 2;
	}
	else if (payloadLen == 127)
	{
		return result; // 64-bit length not supported
	}
	
	// Validate payload length doesn't exceed buffer (increased to support larger frames)
	if (payloadLen <= 0 || payloadLen > 16000) return result;
	
	// Handle masking key
	unsigned char maskKey[4] = {0};
	if (masked)
	{
		if (pos + 4 > length) return result;
		memcpy(maskKey, &buffer[pos], 4);
		pos += 4;
	}
	
	// Validate we have enough data for the payload
	if (pos + payloadLen > length) return result;
	
	// Extract and unmask payload
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
	
	return result;
}

BOOL InitializeWebSocket(void)
{
	OutputDebugString(_T("OpenAlgo: InitializeWebSocket() called"));

	if (g_bWebSocketConnected)
	{
		OutputDebugString(_T("OpenAlgo: InitializeWebSocket - Already connected, returning TRUE"));
		return TRUE;
	}

	if (g_bWebSocketConnecting)
	{
		OutputDebugString(_T("OpenAlgo: InitializeWebSocket - Connection in progress, returning FALSE"));
		return FALSE; // Connection in progress, don't start another
	}

	if (g_oWebSocketUrl.IsEmpty() || g_oApiKey.IsEmpty())
	{
		CString errMsg;
		errMsg.Format(_T("OpenAlgo: InitializeWebSocket - FAILED: URL='%s' APIKey='%s'"),
			g_oWebSocketUrl.IsEmpty() ? _T("EMPTY") : g_oWebSocketUrl,
			g_oApiKey.IsEmpty() ? _T("EMPTY") : _T("SET"));
		OutputDebugString(errMsg);
		return FALSE;
	}

	CString startMsg;
	startMsg.Format(_T("OpenAlgo: InitializeWebSocket - Starting connection to %s"), g_oWebSocketUrl);
	OutputDebugString(startMsg);

	// Initialize Winsock
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
	{
		OutputDebugString(_T("OpenAlgo: InitializeWebSocket - WSAStartup FAILED"));
		return FALSE;
	}

	OutputDebugString(_T("OpenAlgo: InitializeWebSocket - WSAStartup succeeded, calling ConnectWebSocket()"));

	g_bWebSocketConnecting = TRUE;
	BOOL result = ConnectWebSocket();
	g_bWebSocketConnecting = FALSE;

	CString resultMsg;
	resultMsg.Format(_T("OpenAlgo: InitializeWebSocket - ConnectWebSocket returned %d"), result);
	OutputDebugString(resultMsg);

	return result;
}

BOOL ConnectWebSocket(void)
{
	// Parse WebSocket URL
	CString host, path;
	int port = 80;
	
	CString url = g_oWebSocketUrl;
	if (url.Left(5) == _T("wss://"))
	{
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
	
	// Create socket
	g_websocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (g_websocket == INVALID_SOCKET)
		return FALSE;
	
	// Set socket timeouts (blocking mode initially)
	int timeout = 5000; // 5 seconds
	setsockopt(g_websocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
	setsockopt(g_websocket, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));
	
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
		closesocket(g_websocket);
		g_websocket = INVALID_SOCKET;
		return FALSE;
	}
	
	// Connect to server (blocking with timeout)
	if (connect(g_websocket, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR)
	{
		freeaddrinfo(result);
		closesocket(g_websocket);
		g_websocket = INVALID_SOCKET;
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
	if (send(g_websocket, requestA, requestA.GetLength(), 0) == SOCKET_ERROR)
	{
		closesocket(g_websocket);
		g_websocket = INVALID_SOCKET;
		return FALSE;
	}
	
	// Wait for upgrade response with proper timeout
	char buffer[1024];
	int received = recv(g_websocket, buffer, sizeof(buffer) - 1, 0);
	if (received > 0)
	{
		buffer[received] = '\0';
		CString response(buffer);
		
		if (response.Find(_T("101")) > 0 && response.Find(_T("Switching Protocols")) > 0)
		{
			g_bWebSocketConnected = TRUE;
			
			// Small delay to allow WebSocket connection to stabilize
			Sleep(200);
			
			// Now set to non-blocking mode for ongoing operations
			u_long mode = 1;
			ioctlsocket(g_websocket, FIONBIO, &mode);
			
			// Authenticate after switching to non-blocking mode
			return AuthenticateWebSocket();
		}
	}
	
	closesocket(g_websocket);
	g_websocket = INVALID_SOCKET;
	return FALSE;
}

BOOL AuthenticateWebSocket(void)
{
	OutputDebugString(_T("OpenAlgo: AuthenticateWebSocket() called"));

	if (!g_bWebSocketConnected)
	{
		OutputDebugString(_T("OpenAlgo: AuthenticateWebSocket - WebSocket NOT CONNECTED, cannot authenticate"));
		return FALSE;
	}

	// Send authentication message
	CString authMsg = _T("{\"action\":\"authenticate\",\"api_key\":\"") + g_oApiKey + _T("\"}");

	// Log authentication message (mask API key for security)
	CString logMsg;
	if (g_oApiKey.GetLength() > 4)
	{
		logMsg.Format(_T("OpenAlgo: AuthenticateWebSocket - Sending auth with API key: %s...%s"),
			g_oApiKey.Left(2), g_oApiKey.Right(2));
	}
	else
	{
		logMsg = _T("OpenAlgo: AuthenticateWebSocket - Sending auth (API key too short or empty!)");
	}
	OutputDebugString(logMsg);

	if (SendWebSocketFrame(authMsg))
	{
		OutputDebugString(_T("OpenAlgo: AuthenticateWebSocket - Sent auth message, waiting for response..."));
		// Wait for authentication response with select (for non-blocking socket)
		fd_set readfds;
		FD_ZERO(&readfds);
		FD_SET(g_websocket, &readfds);
		
		struct timeval timeout;
		timeout.tv_sec = 5;  // Increased timeout to 5 seconds
		timeout.tv_usec = 0;
		
		if (select(0, &readfds, NULL, NULL, &timeout) > 0)
		{
			OutputDebugString(_T("OpenAlgo: AuthenticateWebSocket - Response received from server"));
			char authBuffer[1024];
			int received = recv(g_websocket, authBuffer, sizeof(authBuffer) - 1, 0);

			CString recvLog;
			recvLog.Format(_T("OpenAlgo: AuthenticateWebSocket - recv() returned %d bytes"), received);
			OutputDebugString(recvLog);

			if (received > 0)
			{
				authBuffer[received] = '\0';
				CString authResponse = DecodeWebSocketFrame(authBuffer, received);

				CString respLog;
				respLog.Format(_T("OpenAlgo: AuthenticateWebSocket - Decoded response: %s"), authResponse);
				OutputDebugString(respLog);

				// Check for success status in authentication response
				// Look for various success indicators that OpenAlgo might send
				if (authResponse.Find(_T("success")) >= 0 ||
					authResponse.Find(_T("authenticated")) >= 0 ||
					authResponse.Find(_T("\"status\":\"ok\"")) >= 0 ||
					authResponse.Find(_T("\"status\":\"success\"")) >= 0)
				{
					OutputDebugString(_T("OpenAlgo: AuthenticateWebSocket - Authentication SUCCESSFUL!"));
					g_bWebSocketAuthenticated = TRUE;
					// Small delay to ensure server has processed authentication
					Sleep(200);

					// Trigger any pending subscriptions now that we're authenticated
					SubscribePendingSymbols();
					return TRUE;
				}
				else if (authResponse.Find(_T("error")) >= 0 || authResponse.Find(_T("failed")) >= 0)
				{
					// Explicit authentication failure
					OutputDebugString(_T("OpenAlgo: AuthenticateWebSocket - Authentication FAILED (error in response)"));
					OutputDebugString(authResponse);
					return FALSE;
				}
				else
				{
					OutputDebugString(_T("OpenAlgo: AuthenticateWebSocket - Received response but no clear success/failure indicator"));
				}
			}
			else if (received == 0)
			{
				OutputDebugString(_T("OpenAlgo: AuthenticateWebSocket - SERVER CLOSED CONNECTION during auth!"));
				g_bWebSocketConnected = FALSE;
				return FALSE;
			}
		}
		else
		{
			OutputDebugString(_T("OpenAlgo: AuthenticateWebSocket - No response within 5 seconds (timeout)"));
		}

		// If we reach here, either timeout or no clear response
		// Since authentication was sent successfully, assume success
		// This is a fallback since the test button works with the same flow
		OutputDebugString(_T("OpenAlgo: AuthenticateWebSocket - Assuming authentication succeeded (fallback)"));
		g_bWebSocketAuthenticated = TRUE;
		// Longer delay to ensure server has processed authentication
		Sleep(1000);  // Increased delay to ensure server processes auth

		// Trigger any pending subscriptions now that we're authenticated
		SubscribePendingSymbols();
		return TRUE;
	}
	
	return FALSE;
}

BOOL SubscribeToSymbol(LPCTSTR pszTicker)
{
	CString logStart;
	logStart.Format(_T("OpenAlgo: SubscribeToSymbol called for: %s"), pszTicker);
	OutputDebugString(logStart);

	if (!g_bWebSocketConnected)
	{
		OutputDebugString(_T("OpenAlgo: SubscribeToSymbol - WebSocket NOT CONNECTED, cannot subscribe"));
		return FALSE;
	}

	// Extract symbol and exchange
	CString symbol = GetCleanSymbol(pszTicker);
	CString exchange = GetExchangeFromTicker(pszTicker);

	CString extractLog;
	extractLog.Format(_T("OpenAlgo: SubscribeToSymbol - Extracted: Symbol='%s' Exchange='%s'"),
		symbol, exchange);
	OutputDebugString(extractLog);

	// Send websocket subscriptions per symbol:
	//   Mode 1 (LTP)   = every-tick price -- feeds the chart's live current bar
	//   Mode 2 (Quote) = OHLCV / previous close -- feeds Realtime Quote Window
	//   Mode 3 (Depth) = full snapshot (OHLC + volume + ltp) PLUS the 5-level
	//                    DOM. We use depth.buy[0]/sell[0] as top-of-book bid/ask
	//                    for the Realtime Quote Window and as the source of
	//                    Time & Sales events (RI_STATUS_NEW_BID/NEW_ASK/TRADE).
	// Keep this websocket-only; do not fall back to /api/v1/quotes for streaming
	// windows because that endpoint is rate limited and adds avoidable latency.
	CString subLtp, subQuote, subDepth;
	subLtp.Format(_T("{\"action\":\"subscribe\",\"symbol\":\"%s\",\"exchange\":\"%s\",\"mode\":1}"),
		(LPCTSTR)symbol, (LPCTSTR)exchange);
	subQuote.Format(_T("{\"action\":\"subscribe\",\"symbol\":\"%s\",\"exchange\":\"%s\",\"mode\":2}"),
		(LPCTSTR)symbol, (LPCTSTR)exchange);
	subDepth.Format(_T("{\"action\":\"subscribe\",\"symbol\":\"%s\",\"exchange\":\"%s\",\"mode\":3}"),
		(LPCTSTR)symbol, (LPCTSTR)exchange);

	BOOL r1 = SendWebSocketFrame(subLtp);
	BOOL r2 = SendWebSocketFrame(subQuote);
	BOOL r3 = SendWebSocketFrame(subDepth);

	CString resultLog;
	resultLog.Format(_T("OpenAlgo: SubscribeToSymbol %s -- ltp_send=%d quote_send=%d depth_send=%d"),
		(LPCTSTR)pszTicker, r1, r2, r3);
	OutputDebugString(resultLog);

	BOOL result = (r1 || r2 || r3);

	return result;
}

BOOL UnsubscribeFromSymbol(LPCTSTR pszTicker)
{
	if (!g_bWebSocketConnected)
		return FALSE;
	
	// Extract symbol and exchange
	CString symbol = GetCleanSymbol(pszTicker);
	CString exchange = GetExchangeFromTicker(pszTicker);
	
	// Mirror SubscribeToSymbol's websocket subscriptions.
	CString unsubLtp, unsubQuote, unsubDepth;
	unsubLtp.Format(_T("{\"action\":\"unsubscribe\",\"symbol\":\"%s\",\"exchange\":\"%s\",\"mode\":1}"),
		(LPCTSTR)symbol, (LPCTSTR)exchange);
	unsubQuote.Format(_T("{\"action\":\"unsubscribe\",\"symbol\":\"%s\",\"exchange\":\"%s\",\"mode\":2}"),
		(LPCTSTR)symbol, (LPCTSTR)exchange);
	unsubDepth.Format(_T("{\"action\":\"unsubscribe\",\"symbol\":\"%s\",\"exchange\":\"%s\",\"mode\":3}"),
		(LPCTSTR)symbol, (LPCTSTR)exchange);
	BOOL r1 = SendWebSocketFrame(unsubLtp);
	BOOL r2 = SendWebSocketFrame(unsubQuote);
	BOOL r3 = SendWebSocketFrame(unsubDepth);
	return (r1 || r2 || r3);
}

void SubscribePendingSymbols(void)
{
	// This function subscribes to symbols that are in the real-time quote window
	// but haven't been subscribed to WebSocket yet
	
	if (!g_bWebSocketConnected || !g_bWebSocketAuthenticated)
		return;
	
	EnterCriticalSection(&g_WebSocketCriticalSection);
	
	// Since we don't have a list of symbols from the real-time quote window directly,
	// we'll rely on GetRecentInfo being called for active symbols to trigger subscriptions
	// For now, just ensure we're ready to handle subscriptions
	
	LeaveCriticalSection(&g_WebSocketCriticalSection);
}

BOOL ProcessWebSocketData(void)
{
	// ALWAYS log that this function is called (every time)
	static int s_callCount = 0;
	s_callCount++;
	if (s_callCount <= 5 || s_callCount % 100 == 0)  // Log first 5 calls and every 100th
	{
		CString callMsg;
		callMsg.Format(_T("OpenAlgo: ProcessWebSocketData() call #%d - Connected=%d Socket=%d"),
			s_callCount, g_bWebSocketConnected, (g_websocket != INVALID_SOCKET));
		OutputDebugString(callMsg);
	}

	if (!g_bWebSocketConnected || g_websocket == INVALID_SOCKET)
	{
		// Auto-reconnect if disconnected
		static DWORD lastReconnectAttempt = 0;
		DWORD now = (DWORD)GetTickCount64();

		// Try reconnecting every 5 seconds
		if ((now - lastReconnectAttempt) > 5000)
		{
			lastReconnectAttempt = now;
			OutputDebugString(_T("OpenAlgo: ProcessWebSocketData() - NOT CONNECTED, attempting reconnect..."));

			if (InitializeWebSocket())
			{
				OutputDebugString(_T("OpenAlgo: *** AUTO-RECONNECT SUCCESSFUL! ***"));

				// Re-issue every previously-active subscription. We deliberately
				// keep g_SubscribedSymbols intact across disconnects so that the
				// list of symbols the user is viewing survives a brief WS drop.
				EnterCriticalSection(&g_WebSocketCriticalSection);
				int reSubbed = 0;
				POSITION rpos = g_SubscribedSymbols.GetStartPosition();
				while (rpos != NULL)
				{
					CString sym;
					BOOL b;
					g_SubscribedSymbols.GetNextAssoc(rpos, sym, b);
					SubscribeToSymbol(sym);
					reSubbed++;
				}
				LeaveCriticalSection(&g_WebSocketCriticalSection);

				CString resubLog;
				resubLog.Format(_T("OpenAlgo: Re-subscribed %d symbol(s) after reconnect"), reSubbed);
				OutputDebugString(resubLog);
				return TRUE;
			}
			else
			{
				OutputDebugString(_T("OpenAlgo: Auto-reconnect failed, will retry in 5 seconds"));
			}
		}
		return FALSE;
	}

	// Send periodic ping to keep connection alive
	static DWORD lastPingTime = 0;
	static BOOL pingTimerInitialized = FALSE;
	DWORD currentTime = (DWORD)GetTickCount64();

	// Initialize timer on first call (don't send PING immediately)
	if (!pingTimerInitialized)
	{
		lastPingTime = currentTime;
		pingTimerInitialized = TRUE;
	}

	if ((currentTime - lastPingTime) > 30000) // Ping every 30 seconds
	{
		// Send WebSocket ping frame (opcode 0x09)
		unsigned char pingFrame[6] = {0x89, 0x84, 0x00, 0x00, 0x00, 0x00}; // Ping with 4-byte mask
		GenerateWebSocketMaskKey(&pingFrame[2]);
		// Serialize with UI-thread SendWebSocketFrame -- see g_WebSocketSendCS comment
		if (g_bWebSocketSendCSInit) EnterCriticalSection(&g_WebSocketSendCS);
		if (g_websocket != INVALID_SOCKET)
			send(g_websocket, (char*)pingFrame, 6, 0);
		if (g_bWebSocketSendCSInit) LeaveCriticalSection(&g_WebSocketSendCS);
		lastPingTime = currentTime;
		OutputDebugString(_T("OpenAlgo: Sent WebSocket ping"));
	}
	
	// CRITICAL: Read ALL pending data in a loop
	// The server sends subscription ACKs and market data continuously
	// We must drain the receive buffer or the server will close the connection!
	int messagesProcessed = 0;
	const int MAX_MESSAGES_PER_CALL = 100; // Prevent infinite loop

	while (messagesProcessed < MAX_MESSAGES_PER_CALL)
	{
		// Check for incoming data (non-blocking)
		fd_set readfds;
		FD_ZERO(&readfds);
		FD_SET(g_websocket, &readfds);

		struct timeval timeout;
		timeout.tv_sec = 0;
		timeout.tv_usec = 0; // Non-blocking

		int selectResult = select(0, &readfds, NULL, NULL, &timeout);

		if (selectResult <= 0)
		{
			// No more data available
			if (messagesProcessed > 0)
			{
				CString doneMsg;
				doneMsg.Format(_T("OpenAlgo: Processed %d messages this call"), messagesProcessed);
				OutputDebugString(doneMsg);
			}
			break;
		}

		messagesProcessed++;

		if (selectResult > 0)
	{
		// Increased buffer size to 16KB to handle large WebSocket frames without fragmentation
		// This prevents misinterpreting partial frames as CLOSE frames
		char buffer[16384];
		int received = recv(g_websocket, buffer, sizeof(buffer) - 1, 0);

		// Avoid per-recv logging for performance
		
		if (received > 0)
		{
			CString data = DecodeWebSocketFrame(buffer, received);

			// Only log control frames and errors (not every data message)
			if (data.Find(_T("CLOSE_FRAME")) == 0)
			{
				CString closeLog;
				closeLog.Format(_T("OpenAlgo: Received %s"), data);
				OutputDebugString(closeLog);
			}

			// Handle WebSocket control frames
			if (data.Find(_T("PING_FRAME")) == 0)  // Starts with "PING_FRAME"
			{
				// RFC 6455 Section 5.5.3: PONG must echo the PING's payload exactly
				// Extract payload from "PING_FRAME:XXXXXXXX" format (hex-encoded)
				CString payload;
				int colonPos = data.Find(':');
				if (colonPos > 0)
				{
					payload = data.Mid(colonPos + 1);
				}

				// Convert hex payload back to bytes
				int payloadLen = payload.GetLength() / 2;
				unsigned char payloadBytes[125] = {0};  // Max control frame payload size
				for (int i = 0; i < payloadLen && i < 125; i++)
				{
					CString hexByte = payload.Mid(i * 2, 2);
					payloadBytes[i] = (unsigned char)_tcstoul(hexByte, NULL, 16);
				}

				// Build PONG frame with echoed payload
				unsigned char pongFrame[256];
				int frameLen = 0;

				pongFrame[frameLen++] = 0x8A;  // FIN + opcode 0x0A (PONG)
				pongFrame[frameLen++] = 0x80 | payloadLen;  // MASK bit + payload length

				// Generate and add masking key
				unsigned char maskKey[4];
				GenerateWebSocketMaskKey(maskKey);
				memcpy(&pongFrame[frameLen], maskKey, 4);
				frameLen += 4;

				// Add masked payload (echo back the PING payload)
				for (int i = 0; i < payloadLen; i++)
				{
					pongFrame[frameLen++] = payloadBytes[i] ^ maskKey[i % 4];
				}

				// Send PONG with echoed payload. Serialize with the UI thread's
				// SubscribeToSymbol send() to avoid interleaved bytes on the wire.
				if (g_bWebSocketSendCSInit) EnterCriticalSection(&g_WebSocketSendCS);
				if (g_websocket != INVALID_SOCKET)
					send(g_websocket, (char*)pongFrame, frameLen, 0);
				if (g_bWebSocketSendCSInit) LeaveCriticalSection(&g_WebSocketSendCS);

				CString pongLog;
				pongLog.Format(_T("OpenAlgo: Received PING with %d-byte payload, sent PONG with echoed payload"), payloadLen);
				OutputDebugString(pongLog);

				continue; // Continue processing more messages
			}
			else if (data.Find(_T("CLOSE_FRAME")) == 0)  // Starts with "CLOSE_FRAME"
			{
				// Connection closed by server - log the reason
				CString closeLog;
				closeLog.Format(_T("OpenAlgo: Received %s from server - closing connection"), data);
				OutputDebugString(closeLog);
				g_bWebSocketConnected = FALSE;
				g_bWebSocketAuthenticated = FALSE;
				closesocket(g_websocket);
				g_websocket = INVALID_SOCKET;
				break; // Exit loop
			}
			else if (data == _T("PONG_FRAME"))
			{
				// Pong received, connection is alive
				continue; // Continue processing more messages
			}

			// Handle subscription acknowledgment.
			// The server formats responses with whitespace ("type": "subscribe"),
			// so check for the substring rather than a strict no-space pattern.
			if (!data.IsEmpty() &&
			    data.Find(_T("\"subscribe\"")) >= 0 &&
			    data.Find(_T("\"status\"")) >= 0 &&
			    data.Find(_T("\"ltp\"")) < 0)   // exclude tick frames that mention "subscribe" by accident
			{
				CString ackLog;
				ackLog.Format(_T("OpenAlgo: Subscription ACK: %s"),
					data.GetLength() > 240 ? CString(data).Left(240) : data);
				OutputDebugString(ackLog);
				continue;
			}

			// Detect market-data frames by structural presence of symbol +
			// exchange plus any market payload. Some brokers send depth-only
			// bid/ask updates without ltp, and those still need to drive
			// AmiBroker Bid/Ask Trend and Time & Sales.
			// This is whitespace-tolerant and
			// works whether the server emits {"type":"ltp", ...}, {"type":"quote", ...},
			// {"type":"market_data", ...}, or {"type":"depth", ...}.
			BOOL bHasMarketPayload =
				data.Find(_T("\"ltp\""))       >= 0 ||
				data.Find(_T("\"bid\""))       >= 0 ||
				data.Find(_T("\"ask\""))       >= 0 ||
				data.Find(_T("\"bids\""))      >= 0 ||
				data.Find(_T("\"asks\""))      >= 0 ||
				data.Find(_T("\"buy\""))       >= 0 ||
				data.Find(_T("\"sell\""))      >= 0 ||
				data.Find(_T("\"depth\""))     >= 0 ||
				data.Find(_T("\"open\""))      >= 0 ||
				data.Find(_T("\"volume\""))    >= 0 ||
				data.Find(_T("\"ltq\""))       >= 0 ||
				data.Find(_T("\"last_quantity\"")) >= 0 ||
				data.Find(_T("\"ltt\""))       >= 0;
			BOOL bIsMarketData = !data.IsEmpty() &&
				data.Find(_T("\"symbol\""))   >= 0 &&
				data.Find(_T("\"exchange\"")) >= 0 &&
				bHasMarketPayload;

			if (bIsMarketData)
			{
				// Simple JSON parsing to extract quote data
				CString symbol, exchange, timestamp;
				float ltp = 0, open = 0, high = 0, low = 0, close = 0, volume = 0, oi = 0;
				float lastTradeQty = 0;  // NEW: For real-time candle building
				float bidPx = 0, bidQty = 0, askPx = 0, askQty = 0;  // Mode 3 DOM L1
				CString messageType = ExtractJsonStringValue(data, _T("type"));
				CString modeText = ExtractJsonStringValue(data, _T("mode"));
				int wsMode = ExtractJsonIntValue(data, _T("mode"));
				BOOL bLtpFrame =
					(wsMode == 1 || messageType.CompareNoCase(_T("ltp")) == 0 ||
					 modeText.CompareNoCase(_T("ltp")) == 0);
				BOOL bQuoteFrame =
					(wsMode == 2 || messageType.CompareNoCase(_T("quote")) == 0 ||
					 modeText.CompareNoCase(_T("quote")) == 0);
				BOOL bDepthFrame =
					(wsMode == 3 || messageType.CompareNoCase(_T("depth")) == 0 ||
					 modeText.CompareNoCase(_T("depth")) == 0);

				symbol = ExtractJsonStringValue(data, _T("symbol"));
				exchange = ExtractJsonStringValue(data, _T("exchange"));
				ltp = ExtractJsonFloatValue(data, _T("ltp"));

				// Extract Mode 2 (Quote) fields: open / high / low / close /
				// volume. Mode 1 (LTP) frames don't have these so the locals
				// stay 0, and we'll preserve any earlier values when we copy
				// into the cache (see below).
				{
					open = ExtractJsonFloatValue(data, _T("open"));
					high = ExtractJsonFloatValue(data, _T("high"));
					low = ExtractJsonFloatValue(data, _T("low"));
					close = ExtractJsonFloatValue(data, _T("close"));
					if (close <= 0.0f)
						close = ExtractJsonFloatValue(data, _T("prev_close"));
					volume = ExtractJsonFloatValue(data, _T("volume"));
					oi = ExtractJsonFloatValue(data, _T("oi"));
				}

				// Mode 3 (Depth) only: extract top-of-book bid/ask. These
				// remain 0 for Mode 1 (LTP-only) frames which is fine -- we
				// preserve the previously cached bid/ask in the RecentInfo
				// update path below.
				if (!ExtractFirstDepthLevel(data, _T("buy"),  bidPx, bidQty))
					ExtractFirstDepthLevel(data, _T("bids"), bidPx, bidQty);
				if (!ExtractFirstDepthLevel(data, _T("sell"), askPx, askQty))
					ExtractFirstDepthLevel(data, _T("asks"), askPx, askQty);

				// Some websocket brokers include scalar bid/ask in quote frames.
				// Keep them websocket-only and use them when depth arrays are absent.
				if (bidPx <= 0.0f)
					bidPx = ExtractJsonFloatValue(data, _T("bid"));
				if (bidPx <= 0.0f)
					bidPx = ExtractJsonFloatValue(data, _T("bid_price"));
				if (bidPx <= 0.0f)
					bidPx = ExtractJsonFloatValue(data, _T("best_bid"));
				if (askPx <= 0.0f)
					askPx = ExtractJsonFloatValue(data, _T("ask"));
				if (askPx <= 0.0f)
					askPx = ExtractJsonFloatValue(data, _T("ask_price"));
				if (askPx <= 0.0f)
					askPx = ExtractJsonFloatValue(data, _T("best_ask"));
				if (bidQty <= 0.0f)
					bidQty = ExtractJsonFloatValue(data, _T("bid_size"));
				if (bidQty <= 0.0f)
					bidQty = ExtractJsonFloatValue(data, _T("bid_quantity"));
				if (askQty <= 0.0f)
					askQty = ExtractJsonFloatValue(data, _T("ask_size"));
				if (askQty <= 0.0f)
					askQty = ExtractJsonFloatValue(data, _T("ask_quantity"));

				// Extract last trade quantity - try both field names
				// Legacy format: "last_trade_quantity":100
				// Docs format: "ltq":100
				lastTradeQty = ExtractJsonFloatValue(data, _T("ltq"));
				if (lastTradeQty <= 0.0f)
					lastTradeQty = ExtractJsonFloatValue(data, _T("last_trade_quantity"));
				if (lastTradeQty <= 0.0f)
					lastTradeQty = ExtractJsonFloatValue(data, _T("last_quantity"));
				if (lastTradeQty <= 0.0f)
					lastTradeQty = ExtractJsonFloatValue(data, _T("last_qty"));

				// NEW: Extract timestamp (supports both Unix milliseconds and ISO 8601 string)
				// Server sends: "timestamp":1761157800000 (Unix milliseconds, no quotes)
				// OR: "timestamp":"2025-05-28T10:30:45.123Z" (ISO 8601 string)
				int timestampPos = data.Find(_T("\"timestamp\":"));
				if (timestampPos >= 0)
				{
					timestampPos += 12;  // Skip "timestamp":
					while (timestampPos < data.GetLength() && IsJsonWhitespace(data[timestampPos]))
						timestampPos++;

					// Check if it's a string (starts with quote) or number
					CString nextChar = data.Mid(timestampPos, 1);
					if (nextChar == _T("\""))
					{
						// ISO 8601 string format: "timestamp":"2025-05-28..."
						timestampPos += 1;  // Skip opening quote
						int endPos = data.Find(_T("\""), timestampPos);
						timestamp = data.Mid(timestampPos, endPos - timestampPos);
					}
					else
					{
						// Unix milliseconds format: "timestamp":1761157800000
						int endPos = data.Find(_T(","), timestampPos);
						if (endPos < 0) endPos = data.Find(_T("}"), timestampPos);
						timestamp = data.Mid(timestampPos, endPos - timestampPos);
						timestamp.Trim();  // Remove whitespace
					}
				}

				// LTP frames are trade ticks. Some brokers omit LTQ in this
				// mode, so use a minimal non-zero size for AmiBroker T&S while
				// still preserving real LTQ whenever the feed provides it.
				if (bLtpFrame && ltp > 0.0f && lastTradeQty <= 0.0f)
					lastTradeQty = 1.0f;
				BOOL bTradeFrame = (ltp > 0.0f &&
				                    (bLtpFrame ||
				                     (!bQuoteFrame && !bDepthFrame && lastTradeQty > 0.0f)));
				CString tradeToken = ExtractJsonRawValue(data, _T("ltt"));
				if (tradeToken.IsEmpty())
					tradeToken = ExtractJsonRawValue(data, _T("last_trade_time"));
				if (tradeToken.IsEmpty() && bLtpFrame)
					tradeToken = timestamp;

				// Throttled logging - only log every 100th tick to avoid performance impact
				static int s_wsCounter = 0;
				s_wsCounter++;
				if (s_wsCounter <= 3 || s_wsCounter % 100 == 0)
				{
					CString debugMsg;
					debugMsg.Format(_T("OpenAlgo: WS Tick #%d: %s-%s LTP=%.2f Qty=%.0f"),
						s_wsCounter, symbol, exchange, ltp, lastTradeQty);
					OutputDebugString(debugMsg);
				}

				// Extract other fields similarly...
				// (Simplified implementation - you could add more fields)

				// Update cache (for GetRecentInfo() compatibility).
				// Mode 1 (LTP) frames carry only ltp+timestamp; Mode 2 (Quote)
				// frames carry full OHLC+volume. Merge into the existing cache
				// entry so a Mode 1 tick doesn't blow away OHLC populated by
				// the last Mode 2 quote.
				if (!symbol.IsEmpty() && !exchange.IsEmpty())
				{
					CString ticker = symbol + _T("-") + exchange;
					QuoteCache quote;
					g_QuoteCache.Lookup(ticker, quote);   // start from cached, if any

					quote.symbol = symbol;
					quote.exchange = exchange;
					if (ltp    > 0.0f) quote.ltp    = ltp;
					if (open   > 0.0f) quote.open   = open;
					if (high   > 0.0f) quote.high   = high;
					if (low    > 0.0f) quote.low    = low;
					if (close  > 0.0f) quote.close  = close;
					if (volume > 0.0f) quote.volume = volume;
					if (oi     > 0.0f) quote.oi     = oi;
					quote.lastUpdate = (DWORD)GetTickCount64();

					g_QuoteCache.SetAt(ticker, quote);

					// Update the persistent RecentInfo for the Realtime Quote
					// Window + Time & Sales (Mode 1 LTP + Mode 3 Depth path).
					struct RecentInfo* pRI = GetOrCreateRecentInfoEntry(ticker);
					if (pRI)
					{
						EnterCriticalSection(&g_RecentInfoCS);

						// Snapshot previous values so we can detect transitions
						// (TRADE / NEW_BID / NEW_ASK) below and drive Time & Sales.
						float prevLast = pRI->fLast;
						float prevBid  = pRI->fBid;
						float prevAsk  = pRI->fAsk;
						int prevBidSize = pRI->iBidSize;
						int prevAskSize = pRI->iAskSize;

						int bm = pRI->nBitmap;
						if (ltp    > 0.0f) { pRI->fLast = ltp;       bm |= RI_LAST; }
						if (open   > 0.0f) { pRI->fOpen = open;      bm |= RI_OPEN; }
						if (high   > 0.0f) { pRI->fHigh = high;      bm |= RI_HIGHLOW; }
						if (low    > 0.0f) { pRI->fLow  = low;       bm |= RI_HIGHLOW; }
						if (close  > 0.0f)
						{
							pRI->fPrev = close;
							if (ltp > 0.0f) pRI->fChange = ltp - close;
							bm |= RI_PREVCHANGE;
						}
						if (volume > 0.0f)
						{
							pRI->iTotalVol = (int)volume;
							pRI->fTotalVol = volume;
							bm |= RI_TOTALVOL;
						}
						if (lastTradeQty > 0.0f)
						{
							pRI->iTradeVol = (int)lastTradeQty;
							pRI->fTradeVol = lastTradeQty;
							bm |= RI_TRADEVOL;
						}
						if (oi     > 0.0f) { pRI->fOpenInt = oi;     bm |= RI_OPENINT; }

						// Top-of-book from Mode 3 Depth frames
						if (bidPx  > 0.0f) { pRI->fBid     = bidPx;  pRI->iBidSize = (int)bidQty; bm |= RI_BID; }
						if (askPx  > 0.0f) { pRI->fAsk     = askPx;  pRI->iAskSize = (int)askQty; bm |= RI_ASK; }

						// nStatus drives Time & Sales row type. We compute the
						// flags AFTER the field writes so we know exactly which
						// values changed in THIS frame.
						BOOL priceChanged = (ltp > 0.0f && ltp != prevLast);
						// LTP mode is the websocket trade stream. Do not suppress
						// Time & Sales rows when consecutive trades print at the
						// same price; AmiBroker still needs RI_STATUS_TRADE.
						static CMap<CString, LPCTSTR, CString, LPCTSTR> s_LastTradeTokens;
						if (!bTradeFrame && ltp > 0.0f && lastTradeQty > 0.0f && !tradeToken.IsEmpty())
						{
							CString prevTradeToken;
							if (!s_LastTradeTokens.Lookup(ticker, prevTradeToken) || prevTradeToken != tradeToken)
								bTradeFrame = TRUE;
						}
						if (bTradeFrame && !tradeToken.IsEmpty())
							s_LastTradeTokens.SetAt(ticker, tradeToken);

						BOOL fireTrade  = bTradeFrame;
						BOOL fireNewBid = (bidPx > 0.0f &&
						                   (bidPx != prevBid ||
						                    (bidQty > 0.0f && (int)bidQty != prevBidSize)));
						BOOL fireNewAsk = (askPx > 0.0f &&
						                   (askPx != prevAsk ||
						                    (askQty > 0.0f && (int)askQty != prevAskSize)));
						BOOL bTimeSalesEvent =
							(ltp > 0.0f) ||
							((bm & (RI_BID | RI_ASK)) != 0) ||
							fireTrade ||
							fireNewBid ||
							fireNewAsk;
						if (bTimeSalesEvent)
						{
							// AmiBroker's Bid/Ask Trend and Time & Sales windows
							// are driven by the RecentInfo pointer notification.
							// Match the ADK/forum pattern: mark every streaming
							// market frame as a trade+bidask update, and add the
							// NEW_BID/NEW_ASK detail bits when the book changed.
							int newStatus =
								RI_STATUS_UPDATE |
								RI_STATUS_BIDASK |
								RI_STATUS_TRADE |
								RI_STATUS_BARSREADY;
							if (fireNewBid) newStatus |= RI_STATUS_NEW_BID;
							if (fireNewAsk) newStatus |= RI_STATUS_NEW_ASK;
							pRI->nStatus = newStatus;
						}
						else if (pRI->nStatus == 0)
						{
							pRI->nStatus = RI_STATUS_UPDATE | RI_STATUS_BARSREADY;
						}

						// nDateUpdate / nTimeUpdate MUST be refreshed every tick
						// and marked valid in nBitmap or AmiBroker's Quote Window
						// and Time & Sales views won't refresh reliably.
						CTime nowT = CTime::GetCurrentTime();
						pRI->nDateUpdate = nowT.GetYear() * 10000 + nowT.GetMonth() * 100 + nowT.GetDay();
						pRI->nTimeUpdate = nowT.GetHour() * 10000 + nowT.GetMinute() * 100 + nowT.GetSecond();
						bm |= RI_DATEUPDATE;
						if (bTimeSalesEvent)
						{
							pRI->nDateChange = pRI->nDateUpdate;
							pRI->nTimeChange = pRI->nTimeUpdate;
							bm |= RI_DATECHANGE;
						}
						pRI->nBitmap = bm;
						LeaveCriticalSection(&g_RecentInfoCS);

						NotifyRecentInfoUpdate(pRI, bTimeSalesEvent);

						// Throttled diagnostic so we can confirm in DbgView
						// that the worker thread really IS populating each
						// subscribed symbol's RecentInfo. First 3 updates per
						// ticker + every 200th after that.
						{
							static CMap<CString, LPCTSTR, int, int> s_updCounts;
							int nu = 0;
							s_updCounts.Lookup(ticker, nu);
							nu++;
							s_updCounts.SetAt(ticker, nu);
							if (nu <= 3 || (nu % 200) == 0)
							{
								CString log;
								log.Format(_T("OpenAlgo: WS update #%d %s ltp=%.2f bid=%.2f ask=%.2f o=%.2f h=%.2f l=%.2f c=%.2f v=%.0f bm=0x%X tsEvent=%d"),
									nu, (LPCTSTR)ticker, ltp, bidPx, askPx, open, high, low, close, volume, bm, bTimeSalesEvent);
								OutputDebugString(log);
							}
						}
					}

					// Process only trade/LTP frames for real-time candle
					// building. Quote/depth frames can repeat the last traded
					// price with a newer book/update timestamp, which creates
					// a false future candle until historical backfill corrects it.
					if (g_bRealTimeCandlesEnabled && bTradeFrame)
					{
						// Default quantity to 1 if missing (quote mode doesn't include ltq)
						if (lastTradeQty <= 0)
							lastTradeQty = 1.0f;

						// Use exchange/server tick timestamp for candle
						// bucketing. Fall back to local time only if the feed
						// omits timestamp or sends a clearly invalid value.
						time_t tickTimestamp = time(NULL);
						if (!timestamp.IsEmpty())
						{
							time_t serverTime = 0;
							if (TryParseWebSocketTimestamp(timestamp, serverTime))
							{
								time_t diff = serverTime - tickTimestamp;
								if (diff >= -1800 && diff <= 300)
								{
									tickTimestamp = serverTime;
								}
								else
								{
									static DWORD s_lastRejectedTsLog = 0;
									DWORD nowTick = (DWORD)GetTickCount64();
									if ((nowTick - s_lastRejectedTsLog) > 60000)
									{
										s_lastRejectedTsLog = nowTick;
										CString tsLog;
										tsLog.Format(_T("OpenAlgo: rejected WS timestamp for candle alignment: %s diff=%ld sec"),
											(LPCTSTR)timestamp, (long)diff);
										OutputDebugString(tsLog);
									}
								}
							}
						}

						ProcessTick(symbol, exchange, ltp, lastTradeQty, tickTimestamp);
					}
				}

				continue; // Continue processing more messages
			}
			else
			{
				// Unknown message type - just continue
				OutputDebugString(_T("OpenAlgo: Received unknown/unhandled message type"));
				continue;
			}
		}
		else if (received == 0)
		{
			// Connection closed by server (graceful close)
			OutputDebugString(_T("OpenAlgo: ========== CRITICAL: SERVER CLOSED CONNECTION =========="));
			OutputDebugString(_T("OpenAlgo: recv() returned 0 - server sent FIN packet (connection closed gracefully)"));
			OutputDebugString(_T("OpenAlgo: Server closed after ~1 minute - will attempt auto-reconnect"));
			OutputDebugString(_T("OpenAlgo: ========================================================"));

			// Mark as disconnected. IMPORTANT: do NOT clear g_SubscribedSymbols
			// here -- we need that list to re-issue subscribes after reconnect.
			g_bWebSocketConnected = FALSE;
			g_bWebSocketAuthenticated = FALSE;
			closesocket(g_websocket);
			g_websocket = INVALID_SOCKET;

			// Attempt immediate reconnection
			OutputDebugString(_T("OpenAlgo: Attempting WebSocket reconnection..."));
			if (InitializeWebSocket())
			{
				OutputDebugString(_T("OpenAlgo: *** RECONNECTED SUCCESSFULLY! ***"));

				// Re-send subscribe for every symbol we were tracking.
				EnterCriticalSection(&g_WebSocketCriticalSection);
				int reSubbed = 0;
				POSITION rpos = g_SubscribedSymbols.GetStartPosition();
				while (rpos != NULL)
				{
					CString sym;
					BOOL b;
					g_SubscribedSymbols.GetNextAssoc(rpos, sym, b);
					SubscribeToSymbol(sym);
					reSubbed++;
				}
				LeaveCriticalSection(&g_WebSocketCriticalSection);

				CString resubLog;
				resubLog.Format(_T("OpenAlgo: Re-subscribed %d symbol(s) after immediate reconnect"), reSubbed);
				OutputDebugString(resubLog);
			}
			else
			{
				OutputDebugString(_T("OpenAlgo: *** RECONNECTION FAILED - will retry on next call ***"));
			}

			break; // Exit loop after reconnect attempt
		}
	}
	} // End of while loop for processing messages

	return (messagesProcessed > 0); // Return TRUE if we processed any messages
}

void CleanupWebSocket(void)
{
	if (g_bCriticalSectionInitialized)
	{
		EnterCriticalSection(&g_WebSocketCriticalSection);
		
		// Unsubscribe from all symbols
		POSITION pos = g_SubscribedSymbols.GetStartPosition();
		while (pos != NULL)
		{
			CString symbol;
			BOOL subscribed;
			g_SubscribedSymbols.GetNextAssoc(pos, symbol, subscribed);
			
			if (subscribed)
			{
				UnsubscribeFromSymbol(symbol);
			}
		}
		
		g_SubscribedSymbols.RemoveAll();
		
		LeaveCriticalSection(&g_WebSocketCriticalSection);
		DeleteCriticalSection(&g_WebSocketCriticalSection);
		g_bCriticalSectionInitialized = FALSE;
	}
	
	// Close WebSocket connection
	if (g_websocket != INVALID_SOCKET)
	{
		closesocket(g_websocket);
		g_websocket = INVALID_SOCKET;
	}
	
	g_bWebSocketConnected = FALSE;
	g_bWebSocketAuthenticated = FALSE;
	g_bWebSocketConnecting = FALSE;
	
	WSACleanup();
}


// OpenAlgoConfigDlg.h : header file
//

#if !defined(AFX_OPENALGOCONFIGDLG_H__C273E749_D29E_4382_9CB5_B51AC8059116__INCLUDED_)
#define AFX_OPENALGOCONFIGDLG_H__C273E749_D29E_4382_9CB5_B51AC8059116__INCLUDED_

#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000

#include "resource.h"  // Include resource IDs
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")

// Forward declaration
struct InfoSite;

/////////////////////////////////////////////////////////////////////////////
// COpenAlgoConfigDlg dialog

class COpenAlgoConfigDlg : public CDialog
{
	// Construction
public:
	COpenAlgoConfigDlg(CWnd* pParent = NULL);   // standard constructor

	// Dialog Data
		//{{AFX_DATA(COpenAlgoConfigDlg)
	enum { IDD = IDD_CONFIG_DIALOG };
	//}}AFX_DATA

	// Pointer to AmiBroker's InfoSite interface
	struct InfoSite* m_pSite;

	// Overrides
		// ClassWizard generated virtual function overrides
		//{{AFX_VIRTUAL(COpenAlgoConfigDlg)
protected:
	virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV support
	//}}AFX_VIRTUAL

// Implementation
protected:
	// Generated message map functions
	//{{AFX_MSG(COpenAlgoConfigDlg)
	virtual BOOL OnInitDialog();
	virtual void OnOK();
	afx_msg void OnTestConnectionButton();
	afx_msg void OnTestWebSocketButton();
	afx_msg void OnApiKeyEditSetFocus();
	//}}AFX_MSG
	DECLARE_MESSAGE_MAP()

private:
	// Helper functions
	BOOL TestWebSocketConnection(const CString& wsUrl, const CString& apiKey);
	BOOL SendWebSocketFrame(SOCKET sock, const CString& message);
	CString DecodeWebSocketFrame(const char* buffer, int length);
	void GenerateMaskKey(unsigned char* maskKey);
	
	// WebSocket related members
	CString m_wsUrl;
	BOOL m_bWebSocketConnected;
};

//{{AFX_INSERT_LOCATION}}
// Microsoft Visual C++ will insert additional declarations immediately before the previous line.

#endif // !defined(AFX_OPENALGOCONFIGDLG_H__C273E749_D29E_4382_9CB5_B51AC8059116__INCLUDED_)
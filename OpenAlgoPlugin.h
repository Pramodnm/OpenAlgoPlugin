// OpenAlgoPlugin.h : main header file for the OpenAlgo DLL
//

#if !defined(AFX_OPENALGOPLUGIN_H__728CE6DF_A647_40EF_8085_77A8FAE4E133__INCLUDED_)
#define AFX_OPENALGOPLUGIN_H__728CE6DF_A647_40EF_8085_77A8FAE4E133__INCLUDED_

#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000

#ifndef __AFXWIN_H__
#error include 'stdafx.h' before including this file for PCH
#endif

#include "resource.h"		// main symbols

/////////////////////////////////////////////////////////////////////////////
// COpenAlgoApp
// See OpenAlgoPlugin.cpp for the implementation of this class
//

class COpenAlgoApp : public CWinApp
{
public:
	COpenAlgoApp();

	// Public wrapper around the protected CWinApp::SetRegistryKey so non-member
	// callers (Init/Notify/OnOK) can force m_pszRegistryKey to be set even when
	// MFC didn't run InitInstance (Regular MFC DLLs often skip it).
	void EnsureRegistryRoot(LPCTSTR pszKey = _T("OpenAlgo")) { SetRegistryKey(pszKey); }

	// Overrides
	// ClassWizard generated virtual function overrides
	//{{AFX_VIRTUAL(COpenAlgoApp)
public:
	virtual BOOL InitInstance();
	//}}AFX_VIRTUAL

	//{{AFX_MSG(COpenAlgoApp)
		// NOTE - the ClassWizard will add and remove member functions here.
		//    DO NOT EDIT what you see in these blocks of generated code !
	//}}AFX_MSG
	DECLARE_MESSAGE_MAP()
};

// Global App Object
extern COpenAlgoApp theApp;

/////////////////////////////////////////////////////////////////////////////

//{{AFX_INSERT_LOCATION}}
// Microsoft Visual C++ will insert additional declarations immediately before the previous line.

#endif // !defined(AFX_OPENALGOPLUGIN_H__728CE6DF_A647_40EF_8085_77A8FAE4E133__INCLUDED_)
// dummyz@126.com

#include "stdafx.h"
#include "PEJudger.h"
#include "PEResource.h"
#include "zlibcrc32/crc32.h"

#ifdef _PEDISAM_
#include "PEDisasm.h"
#endif

class COpResult
{
public:
	COpResult(BOOL (* op)(CPEParser* lpParser), CPEParser* lpParser)
	{
		m_nResult = -1;
		m_op = op;
		m_lpParser = lpParser;
	}

	operator BOOL ()
	{
		if ( m_nResult == -1 )
		{
			if ( m_op(m_lpParser) )
			{
				m_nResult = TRUE;
			}
			else
			{
				m_nResult = FALSE;
			}
		}
		
		return m_nResult;
	}

protected:
	BOOL		(* m_op)(CPEParser* lpParser);
	int			m_nResult;
	CPEParser*	m_lpParser;
};

//////////////////////////////////////////////////////////////////////////

CPEJudger::CPEJudger()
{

}

CPEJudger::~CPEJudger()
{

}

DWORD CPEJudger::GuessRiskLevel(CPEParser* lpParser, LPTSTR lpszName)
{
	if ( lpParser->IsPEPlus() )
	{
		//下面规则不适用于peplus
		return 0;
	}

	if ( CPEiDentifier::IsResourceBin(lpParser) )
	{
		return 0;
	}

	if ( __IsSkipFile(lpParser) )
	{
		return 0;
	}

	DWORD dwResult = 0;
	LPCTSTR lpClass = NULL;
	IPEFile* lpFile = lpParser->GetFileObject();
	BOOL bExe = lpParser->IsExe();
	COpResult bFilePacked(CPEiDentifier::IsSectionPacked, lpParser);
//	COpResult bResourcePacked(CPEiDentifier::IsResourcePacked, lpParser);
//	COpResult bImportPacked(CPEiDentifier::IsImportPacked, lpParser);
	COpResult bOverlayBindExe(IsOverlayBindExe, lpParser);
	COpResult bResourceBindExe(IsResourceBindExe, lpParser);

	//
	// 顺序不要随便调整
	//
	if ( CPEiDentifier::IsRiskPacker(lpParser) )
	{
		lpClass = _T("Packer");
		dwResult = MAKELONG(RiskLevelHig, RiskTypeTrojan);
		goto _Exit1;
	}

	if ( bExe && IsRiskIcon(lpParser) )
	{
		lpClass = _T("Crook.Icon");
		dwResult = MAKELONG(RiskLevelHig, RiskTypeTrojan);
		goto _Exit1;
	}

	// 检测感染
	DWORD nScore = GuessPackerOrVirus(lpParser);
	if ( nScore > 2 )
	{
		DWORD dwType = (HIWORD(nScore) != 0) ? RiskTypeVirus : RiskTypeTrojan;

		if ( bOverlayBindExe || bResourceBindExe )
		{
			lpClass = _T("Bxx");
			dwResult = MAKELONG(RiskLevelHig, dwType);
		}

		// LOWORD(nScore) 大于 10 是明确的壳类型
		if ( bExe && LOWORD(nScore) < 10 && !bFilePacked )
		{
			lpClass = _T("Infect");
			dwResult = MAKELONG(RiskLevelHig, dwType);
			goto _Exit1;
		}

		if ( bExe && IsSingleIcon(lpParser) )
		{
			lpClass = _T("Sxx");
			dwResult = MAKELONG(RiskLevelLow, dwType);
			goto _Exit1;
		}

		DWORD dwFileAttr = lpFile->GetAttr();
		if ( dwFileAttr & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM) )
		{
			lpClass = _T("Axx");
			dwResult = MAKELONG(RiskLevelMid, dwType);
			goto _Exit1;
		}
	}

	// 不去尝试反汇编加壳文件, 不包括部分免杀型 upx
	if ( !bFilePacked && LOWORD(nScore) < 10 )
	{
#ifdef _PEDISAM_
		// 入口点
		DWORD nOepScore = GuessVirusByOep(lpParser);
		if ( nOepScore >= 3 )
		{
			lpClass = _T("Oxx");
			dwResult = MAKELONG(RiskLevelMid, RiskTypeTrojan);
			goto _Exit1;
		}
#endif
	}

	// 检测伪装文件
	BOOL bNotPacker = FALSE;
	DWORD dwLinker = 0;
	if ( CPEiDentifier::GetLinkerInfo(lpParser, bNotPacker, dwLinker) )
	{
		if ( LOWORD(nScore) > 2 && bNotPacker )
		{
			lpClass = _T("Crook.Packer");
			dwResult =  MAKELONG(RiskLevelHig, RiskTypeTrojan);
			goto _Exit1;
		}

		if ( (dwLinker & VB_Linker) == 0 && CPEiDentifier::IsVBLink(lpParser) )
		{
			lpClass = _T("Crook.VB");
			dwResult = MAKELONG(RiskLevelHig, RiskTypeTrojan);
			goto _Exit1;
		}

		if ( (dwLinker & Delphi_Linker) == 0 && CPEiDentifier::IsDelphiLink(lpParser) )
		{
			lpClass = _T("Crook.Delphi");
			dwResult = MAKELONG(RiskLevelHig, RiskTypeTrojan);
			goto _Exit1;
		}

		if ( (dwLinker & Eyuyan_Linker) == 0 && CPEiDentifier::IsEyuyanLink(lpParser) )
		{
			lpClass = _T("Crook.Eyuyan");
			dwResult = MAKELONG(RiskLevelHig, RiskTypeTrojan);
			goto _Exit1;
		}
	}


_Exit1:
	if ( dwResult != 0 )
	{
		LPCTSTR lpTypeName = NULL;
		switch ( HIWORD(dwResult) )
		{
		case RiskTypeTrojan:
			lpTypeName = _T("Trojan");
			break;

		case RiskTypeVirus:
			lpTypeName = _T("Virus");
			break;

		case RiskTypeUnknown:
			lpTypeName = _T("Unknown");
			break;
		}

		_stprintf_s(lpszName, MAX_PATH, _T("Heur:%s.%s.%d"), lpTypeName, lpClass, LOWORD(dwResult));
	}

	return dwResult;
}

DWORD CPEJudger::GuessPackerOrVirus(CPEParser* lpParser)
{
	ULONG nScorePacker = 0;
	ULONG nScoreVirus = 0;

	ULONG lEntryPoint = lpParser->GetEntryPoint();
	if ( lEntryPoint != 0 && lEntryPoint != EOF )
	{
		PIMAGE_SECTION_HEADER lpSectHead = lpParser->GetSectionHeadByOffset(lEntryPoint);
		if ( lpSectHead != NULL )
		{
			DWORD dwSectionCount = lpParser->GetSectionCount();
			PIMAGE_SECTION_HEADER lpFirstSectHead = lpParser->GetSectionHead(0);
			PIMAGE_SECTION_HEADER lpLastSectHead = lpParser->GetSectionHead(dwSectionCount - 1) ;
			PIMAGE_NT_HEADERS lpNtHead = lpParser->GetNtHead();

			// 入口点在代码端之外+
			if ( lEntryPoint < lpNtHead->OptionalHeader.BaseOfCode ||
				lEntryPoint >= lpNtHead->OptionalHeader.SizeOfCode + lpNtHead->OptionalHeader.BaseOfCode
				)
			{
				if ( lpLastSectHead - lpSectHead >=  2 )
				{

				}
				else if ( lpLastSectHead - lpSectHead == 1 )
				{
					nScorePacker++;
				}
				else
				{
					nScorePacker += 2;
				}
			}

			// 入口点在最后一个节
			if ( dwSectionCount > 2 && lpSectHead == lpLastSectHead )
			{
				nScorePacker += 2;
			}

			// 有写权限
			if ( 0 != (lpSectHead->Characteristics & IMAGE_SCN_MEM_WRITE) )
			{
				nScorePacker++;
			}

			if ( lpFirstSectHead != lpSectHead )
			{
				if ( memcmp(lpFirstSectHead->Name, ".text\0\0\0", 8) == 0 ||
					memcmp(lpFirstSectHead->Name, "CODE\0\0\0\0", 8) == 0
					)
				{
					if ( lpSectHead == lpLastSectHead )
					{
						nScoreVirus += 3;
					}
				}

				if ( lpSectHead->VirtualAddress == lEntryPoint )
				{
					nScorePacker++;
					if ( lpSectHead == lpLastSectHead )
					{
						nScoreVirus += 3;
					}
				}
			}
			
			if ( lpSectHead != lpLastSectHead )
			{
				const DWORD Characteristics = IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_WRITE;
				if ( (lpSectHead->Characteristics & Characteristics) == Characteristics &&
					(lpLastSectHead->Characteristics & Characteristics) == Characteristics
					)
				{
					nScoreVirus += 3;
				}
			}

			// 过滤掉明确的壳类型
			if ( CPEiDentifier::IsNormalPacker(lpParser) )
			{
				nScoreVirus &= 0xFFFF;
				nScorePacker += 10;
			}
		}
		else
		{
			nScoreVirus = 1;
			nScorePacker = 1;
		}
	}

	return MAKELONG(nScorePacker, nScoreVirus);
}

static DWORD GetCallType(CPEParser* lpParser, ULONG lCallTarget)
{
	IPEFile* lpFile = lpParser->GetFileObject();
	ULONG lOffset = lCallTarget - (ULONG)lpParser->GetImageBase();
	ULONG lFilePointer = lpParser->ToFilePointer(lOffset);
	BYTE cBuff[10];

	if ( lFilePointer != EOF &&
		lpFile->Seek(lFilePointer, FILE_BEGIN) == lFilePointer &&
		lpFile->Read(cBuff, 10)
		)
	{
		if ( cBuff[0] == 0xFF && cBuff[1] == 0x25 )
		{
			// may api thunk call
			return 1;
		}
	}

	return 0;
}

#ifdef _PEDISAM_
DWORD CPEJudger::GuessVirusByOep(CPEParser* lpParser)
{
	// 支持 jmp 乱序，push imm32/ret 跳转
	// 不支持花指令分析
	// 不支持假跳转分析
	IPEFile* lpFile = lpParser->GetFileObject();
	ULONG lEntryPoint = lpParser->GetEntryPoint();
	if ( lEntryPoint == 0 || lEntryPoint == EOF )
	{
		return 0;
	}

	ULONG lFilePointer = lpParser->ToFilePointer(lEntryPoint);
	if ( lFilePointer == EOF )
	{
		return 0;
	}

	CPEDiasm diasm(lpParser->IsPEPlus() ? 64 : 32);
	DWORD	dwMemCount = 0;
	DWORD	dwNopCount = 0;
	DWORD	dwTraceJmpCount = 0;
	BOOL	bFixSelf = FALSE;
	BOOL	bTraceJmp = TRUE;
	BOOL	bTraceNop = TRUE;
	BOOL	bContinue = TRUE;
	const ULONG lBaseAddr = (ULONG)lpParser->GetImageBase();
	const ULONG lBaseAddrLimit = lBaseAddr + (ULONG)lpParser->GetImageSize();
	ULONG lPrevEip =  lBaseAddr + lEntryPoint;
	ULONG lCallTarget = 0, lRetTarget = 0;
	
	diasm.Attach(lpFile, lPrevEip, lFilePointer);
	for ( DWORD i = 0; bContinue && i < 50; i++ )
	{
		ud* u = diasm.Disasm();
		if ( u == NULL || u->inp_ctr == 0 )
		{
			break;
		}

		ULONG lTempRetTarget = 0;
		switch ( u->mnemonic )
		{
		case UD_Inop:
			if ( bTraceNop )
			{
				dwNopCount++;
			}
			break;

		case UD_Icall:
			lCallTarget = 0;
			if ( u->operand[0].type == UD_OP_JIMM && u->operand[0].size == 32 )
			{
				lCallTarget = (ULONG)u->pc + u->operand[0].lval.sdword;
				if ( GetCallType(lpParser, lCallTarget) == 1 )
				{
					dwMemCount++;
				}
			}
			else if ( u->operand[0].type == UD_OP_MEM && u->operand[0].type == UD_NONE )
			{
				dwMemCount++;
			}
			break;

		case UD_Ipop:
			if ( lCallTarget == lPrevEip )
			{
				bFixSelf = TRUE;
				lCallTarget = 0;
			}
			break;

		case UD_Ipopad:
		case UD_Ipushad:
		case UD_Ipushfd:
		case UD_Ipushfw:
		case UD_Ipopfd:
		case UD_Ipopfw:
			lCallTarget = 0;
			break;

		case UD_Ipush:
			lCallTarget = 0;
			if ( bTraceJmp )
			{
				if ( u->operand[0].type == UD_OP_IMM &&
					u->operand[0].size == 32 
					)
				{
					ULONG lOffset = u->operand[0].lval.udword;
					if ( lOffset > lBaseAddr && lOffset < lBaseAddrLimit )
					{
						lTempRetTarget = lOffset;
						dwMemCount++;
					}
				}
			}
			break;

		case UD_Ijmp:
			if ( bTraceJmp )
			{
				if ( u->operand[0].type == UD_OP_JIMM )
				{
					if ( u->operand[0].size == 32 )
					{
						DWORD dwJmpTarget = (DWORD)u->pc + u->operand[0].lval.sdword;
						bContinue = diasm.GotoEip(dwJmpTarget);
						dwTraceJmpCount++;
					}
					else if ( u->operand[0].size == 8 )
					{
						DWORD dwJmpTarget = (DWORD)u->pc + u->operand[0].lval.sbyte;
						bContinue = diasm.GotoEip(dwJmpTarget);
						dwTraceJmpCount++;
					}
					else
					{
						bTraceJmp = FALSE;
					}
				}
				else
				{
					bTraceJmp = FALSE;
				}
			}
			break;

		case UD_Iret:
			if ( bTraceJmp )
			{
				if ( lRetTarget != 0 &&
					u->operand[0].type == UD_NONE
					)
				{
					bContinue = diasm.GotoEip(lRetTarget);
					dwTraceJmpCount++;
				}

				if ( lCallTarget != 0 )
				{
					bContinue = diasm.GotoEip(lCallTarget);
				}
			}
			lCallTarget = 0;
			break;

		case UD_Ijo:
		case UD_Ijno:
		case UD_Ijb:
		case UD_Ijae:
		case UD_Ijz:
		case UD_Ijnz:
		case UD_Ijbe:
		case UD_Ija:
		case UD_Ijs:
		case UD_Ijns:
		case UD_Ijp:
		case UD_Ijnp:
		case UD_Ijl:
		case UD_Ijge:
		case UD_Ijle:
		case UD_Ijg:
		case UD_Ijcxz:
		case UD_Ijecxz:
		case UD_Ijrcxz:
			bTraceJmp = FALSE;
			break;
		}

		if ( u->mnemonic == UD_Iret || 
			u->mnemonic == UD_Ijmp ||
			strcmp(u->insn_hexcode, "0000") == 0
			)
		{
			bTraceNop = FALSE;
		}
		else if ( u->mnemonic != UD_Inop )
		{
			bTraceNop = TRUE;
		}

		for ( int i = 0; i < 3; i++ )
		{
			if ( u->operand[i].type == UD_OP_MEM &&
				u->operand[i].lval.udword > lBaseAddr &&
				u->operand[i].lval.udword < lBaseAddrLimit
				)
			{
				dwMemCount++;
				break;
			}
		}

		lRetTarget = lTempRetTarget;
		if ( lCallTarget != 0 && lCallTarget == lPrevEip )
		{
			lCallTarget = (ULONG)u->pc;
		}

		lPrevEip = (ULONG)u->pc;
	}
	diasm.Detach();

	DWORD dwSocre = 0;
	if ( dwNopCount > 3 ) 
	{
		dwSocre += 2;
		if ( dwTraceJmpCount >= 2 )
		{
			dwSocre += 2;
		}

		if ( dwMemCount == 0 && dwNopCount > 8 )
		{
			dwSocre += 1;
		}
	}

	if ( bFixSelf ) 
	{
		dwSocre += 3;
		if ( dwTraceJmpCount >= 2 )
		{
			dwSocre += 2;
		}

		if ( dwMemCount == 0 )
		{
			dwSocre += 2;
		}
	}

	return dwSocre;
}
#endif

BOOL CPEJudger::IsSingleIcon(CPEParser* lpParser)
{
	class CResIconEnumer
	{
	private:
		BOOL WINAPI _EnumTypeFunc(LPCWSTR lpType, ULONG lOffset, LPARAM lParam)
		{
			if ( IS_INTRESOURCE(lpType) )
			{
				if ( MAKEINTRESOURCE(lpType) == RT_GROUP_ICON )
				{
					m_nIconGroupCount++;
				}
			}

			m_nTypeCount++;
			return TRUE;
		}

	public:
		static BOOL WINAPI EnumTypeFunc(PIMAGE_RESOURCE_DIRECTORY lpDir, LPCWSTR lpName, ULONG lOffset, LPARAM lParam)
		{
			CResIconEnumer* _this = (CResIconEnumer*)LongToPtr(lParam);
			return _this->_EnumTypeFunc(lpName, lOffset, lParam);
		}

	public:
		CPEParser*	m_lpPEParser;
		ULONG		m_nIconGroupCount;
		ULONG		m_nTypeCount;
	};

	CResIconEnumer enumer;

	enumer.m_nTypeCount = 0;
	enumer.m_nIconGroupCount = 0;
	enumer.m_lpPEParser = lpParser;

	lpParser->EnumResourceTypes(CResIconEnumer::EnumTypeFunc, PtrToLong(&enumer));

	return (enumer.m_nIconGroupCount == 1) && (enumer.m_nTypeCount == 1);
}

BOOL CPEJudger::IsRiskIcon(CPEParser* lpParser)
{
	const ULONG IconGroupOffset = lpParser->FindResourceType(MAKEINTRESOURCEW(RT_GROUP_ICON));
	if ( IconGroupOffset == EOF )
	{
		return FALSE;
	}

	const ULONG IconsOffset = lpParser->FindResourceType(MAKEINTRESOURCEW(RT_ICON));
	if ( IconsOffset == EOF )
	{
		return FALSE;
	}

	class CResIconEnumer
	{
	private:
		BOOL WINAPI _EnumNameFunc(LPCTSTR lpName, ULONG lOffset, DWORD dwSize, DWORD dwCodePage, LPARAM lParam)
		{
			IPEFile* lpFile = m_lpPEParser->GetFileObject();
			ULONG lFilePointer = m_lpPEParser->ToFilePointer(lOffset);

			if ( m_lpPEParser->IsVaildFilePointer(lFilePointer) &&
				m_lpPEParser->IsVaildFilePointer(lFilePointer + dwSize) 
				)
			{
				m_lIconGroupPointer = lFilePointer;
				m_lIconGroupSize = dwSize;

				return FALSE;
			}

			return TRUE;
		}

	public:
		static BOOL WINAPI EnumNamesFunc(PIMAGE_RESOURCE_DIRECTORY lpDir, LPCTSTR lpName, ULONG lOffset, DWORD dwSize, DWORD dwCodePage, LPARAM lParam)
		{
			CResIconEnumer* _this = (CResIconEnumer*)LongToPtr(lParam);
			return _this->_EnumNameFunc(lpName, lOffset, dwSize, dwCodePage, lParam);
		}

	public:
		CPEParser*	m_lpPEParser;
		ULONG		m_lIconGroupPointer;
		ULONG		m_lIconGroupSize;
	};

	CResIconEnumer enumer;

	enumer.m_lpPEParser = lpParser;
	enumer.m_lIconGroupPointer = 0;
	enumer.m_lIconGroupSize = 0;

	lpParser->EnumResourceNames(IconGroupOffset, CResIconEnumer::EnumNamesFunc, PtrToLong(&enumer));
	if ( enumer.m_lIconGroupPointer == 0 || enumer.m_lIconGroupSize == 0 )
	{
		return FALSE;
	}

	IPEFile* lpFile = lpParser->GetFileObject();
	if ( lpFile->Seek(enumer.m_lIconGroupPointer, FILE_BEGIN) != enumer.m_lIconGroupPointer )
	{
		return FALSE;
	}

	PE_ICONDIR IconDir;
	BOOL bResult = FALSE;

	if ( lpFile->ReadT(IconDir.IdReserved) &&
		IconDir.IdReserved == 0 &&
		lpFile->ReadT(IconDir.IdType) &&
		IconDir.IdType == 1 &&
		lpFile->ReadT(IconDir.IdCount) &&
		IconDir.IdCount != 0 
		)
	{
		const DWORD IdCount = min(IconDir.IdCount, 20);
		PE_ICONDIRENTRY* lpIconDirEntry = new PE_ICONDIRENTRY[IdCount];

		DWORD dwReadBytes = IdCount * sizeof (PE_ICONDIRENTRY);
		if ( lpFile->Read(lpIconDirEntry, dwReadBytes) == dwReadBytes )
		{
			static const struct {
				DWORD	dwSize;
				DWORD	dwCRC32;
				DWORD	dwK;
			} __RiskIconCRC32[] = {
				// 文件夹图标
				{ 0x8A8,	0x224C1312, 10 },
				{ 0x6C8,	0xD0FD743E, 10 },
				{ 0xEA8,	0x66F01724, 10 },
				{ 0x10A8,	0xD554A297, 10 },
				// 图片
				{ 0xEA8,	0xD92C630E,	10 },
				{ 0x2E8,	0xC2C54642,	10 },
				{ 0x8A8,	0xD0A6F916,	10 },
				{ 0x2E8,	0x10A547D3,	10 },
				{ 0xEA8,	0x76F5E42D,	10 },
				{ 0x8A8,	0x3A719C94, 10 },
				{ 0x1A8,	0x2D833DD3,	10 },
				{ 0x10A8,	0x2D8F633C, 10 }
			};
			const DWORD dwMaxIconDataSize = 0x5000;
			PVOID lpIconDataBuff = new BYTE[dwMaxIconDataSize];
			DWORD dwKs = 0;

			for ( DWORD i = 0; i < IdCount; i++ )
			{
				ULONG IconDataOffset;
				DWORD dwIconDataSize;

				IconDataOffset = lpParser->FindResourceName(IconsOffset, MAKEINTRESOURCEW(lpIconDirEntry[i].wIconID), 0, &dwIconDataSize);
				if ( IconDataOffset != EOF && dwIconDataSize < dwMaxIconDataSize )
				{
					DWORD dwIconDataCRC32 = 0;
					BOOL bCalcCRC32 = FALSE;

					for ( DWORD j = 0; j < ARRAYSIZE(__RiskIconCRC32); j++ )
					{
						if ( __RiskIconCRC32[j].dwSize == dwIconDataSize )
						{
							if ( !bCalcCRC32 )
							{
								bCalcCRC32 = TRUE;

								ULONG lFilePointer = lpParser->ToFilePointer(IconDataOffset);
								if ( lFilePointer != EOF &&
									lpFile->Seek(lFilePointer, FILE_BEGIN) == lFilePointer &&
									lpFile->Read(lpIconDataBuff, dwIconDataSize) == dwIconDataSize
									)
								{
									dwIconDataCRC32 = CRC32(0, lpIconDataBuff, dwIconDataSize);
								}
								else
								{
									break;
								}
							}
							
							if ( dwIconDataCRC32 == __RiskIconCRC32[j].dwCRC32 )
							{
								dwKs += __RiskIconCRC32[j].dwK;
								break;
							}
						}
					}

					if ( dwKs >= 10 )
					{
						bResult = TRUE;
						break;
					}
				}
			}

			delete lpIconDataBuff;
			lpIconDataBuff = NULL;
		}
	}

	return bResult;
}

BOOL CPEJudger::IsOverlayBindExe(CPEParser* lpParser) // 附加数据捆绑 exe
{
	DWORD dwSize = 0;
	
	ULONG lFilePointer = lpParser->GetOverlayOffset(&dwSize);
	if ( lFilePointer != EOF && dwSize > 0x1000 )
	{
		return ScanPEStruct(lpParser, lFilePointer);
	}

	return FALSE;
}

BOOL CPEJudger::IsResourceBindExe(CPEParser* lpParser) // 资源数据捆绑 exe
{
	class CResDataEnumer
	{
	private:
		BOOL WINAPI _EnumTypeFunc(LPCWSTR lpType, ULONG lOffset, LPARAM lParam)
		{
			m_lpPEParser->EnumResourceNames(lOffset, EnumNamesFunc, lParam);
			return !m_bFoundExe;
		}

		BOOL WINAPI _EnumNameFunc(LPCTSTR lpName, ULONG lOffset, DWORD dwSize, DWORD dwCodePage, LPARAM lParam)
		{
			if ( dwSize > 0x1000 )
			{
				ULONG lFilePointer = m_lpPEParser->ToFilePointer(lOffset);

				m_bFoundExe = CPEJudger::ScanPEStruct(m_lpPEParser, lFilePointer);
				return !m_bFoundExe;
			}

			return TRUE;
		}

	public:
		static BOOL WINAPI EnumTypeFunc(PIMAGE_RESOURCE_DIRECTORY lpDir, LPCWSTR lpName, ULONG lOffset, LPARAM lParam)
		{
			CResDataEnumer* _this = (CResDataEnumer*)LongToPtr(lParam);
			return _this->_EnumTypeFunc(lpName, lOffset, lParam);
		}

		static BOOL WINAPI EnumNamesFunc(PIMAGE_RESOURCE_DIRECTORY lpDir, LPCTSTR lpName, ULONG lOffset, DWORD dwSize, DWORD dwCodePage, LPARAM lParam)
		{
			CResDataEnumer* _this = (CResDataEnumer*)LongToPtr(lParam);
			return _this->_EnumNameFunc(lpName, lOffset, dwSize, dwCodePage, lParam);
		}

	public:
		CPEParser*	m_lpPEParser;
		BOOL		m_bFoundExe;
	};

	CResDataEnumer enumer;

	enumer.m_bFoundExe = FALSE;
	enumer.m_lpPEParser = lpParser;

	lpParser->EnumResourceTypes(CResDataEnumer::EnumTypeFunc, PtrToLong(&enumer));

	return enumer.m_bFoundExe;
}

BOOL CPEJudger::ScanPEStruct(CPEParser* lpParser, ULONG lFilePointer)
{
	BOOL bResult = FALSE;
	IPEFile* lpFile = lpParser->GetFileObject();

	if ( lFilePointer != EOF &&
		lFilePointer == lpFile->Seek(lFilePointer, FILE_BEGIN)
		)
	{
		const DWORD nBuffLen = 0x100;
		BYTE cBuff[nBuffLen + sizeof (IMAGE_DOS_HEADER)] = { 0 };

		if ( nBuffLen == lpFile->Read(cBuff, nBuffLen) )
		{
			for ( int i = 0; i < nBuffLen; i++ )
			{
				if ( cBuff[i] == 'M' && cBuff[i + 1] == 'Z' )
				{
					PIMAGE_DOS_HEADER lpDosHead = (PIMAGE_DOS_HEADER)(cBuff + i);
					if ( lpDosHead->e_lfanew != 0 )
					{
						PIMAGE_NT_HEADERS lpNtHead = (PIMAGE_NT_HEADERS)((PBYTE)lpDosHead + lpDosHead->e_lfanew);
						if ( (PBYTE)lpNtHead > cBuff && (PBYTE)lpNtHead < cBuff + nBuffLen )
						{
							if ( lpNtHead->Signature == IMAGE_NT_SIGNATURE )
							{
								bResult = TRUE;
								break;
							}
						}
					}
				}
			}
		}
	}

	return bResult;
}

BOOL	CPEJudger::__IsSkipFile(CPEParser* lpParser)
{
	DWORD dwSectionCount = lpParser->GetSectionCount();
	PIMAGE_SECTION_HEADER lpFirstSectHead = lpParser->GetSectionHead(0);
	PIMAGE_NT_HEADERS lpNtHead = lpParser->GetNtHead();

	if ( dwSectionCount == 4 )
	{
		// __BOX__
		if ( lpFirstSectHead->SizeOfRawData == 0 && 
			lpFirstSectHead->PointerToRawData == 0 &&
			lpNtHead->OptionalHeader.AddressOfEntryPoint == lpFirstSectHead[dwSectionCount - 1].VirtualAddress &&
			memcmp(lpFirstSectHead[dwSectionCount - 1].Name, "_BOX_\0\0\0", 8) == 0 &&
			memcmp(lpFirstSectHead->Name, "UPX0\0\0\0\0", 8) == 0
			)
		{
			return TRUE;
		}
	}

	return FALSE;
}
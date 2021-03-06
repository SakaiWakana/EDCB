#include "StdAfx.h"
#include "EpgDBUtil.h"

#include "../../Common/StringUtil.h"
#include "../../Common/TimeUtil.h"
#include "../../Common/BlockLock.h"
#include "ARIB8CharDecode.h"

//#define DEBUG_EIT
#ifdef DEBUG_EIT
static char g_szDebugEIT[128];
#endif

namespace Desc = AribDescriptor;

CEpgDBUtil::CEpgDBUtil(void)
{
	InitializeCriticalSection(&this->dbLock);
}

CEpgDBUtil::~CEpgDBUtil(void)
{
	DeleteCriticalSection(&this->dbLock);
}

void CEpgDBUtil::Clear()
{
	this->serviceEventMap.clear();
}

void CEpgDBUtil::SetStreamChangeEvent()
{
	//ここで[p/f]のリセットはしない
}

BOOL CEpgDBUtil::AddEIT(WORD PID, const Desc::CDescriptor& eit, __int64 streamTime)
{
	CBlockLock lock(&this->dbLock);

	WORD original_network_id = (WORD)eit.GetNumber(Desc::original_network_id);
	WORD transport_stream_id = (WORD)eit.GetNumber(Desc::transport_stream_id);
	WORD service_id = (WORD)eit.GetNumber(Desc::service_id);
	ULONGLONG key = _Create64Key(original_network_id, transport_stream_id, service_id);

	//サービスのmapを取得
	map<ULONGLONG, SERVICE_EVENT_INFO>::iterator itr;
	SERVICE_EVENT_INFO* serviceInfo = NULL;

	itr = serviceEventMap.find(key);
	if( itr == serviceEventMap.end() ){
		serviceInfo = &serviceEventMap.insert(std::make_pair(key, SERVICE_EVENT_INFO())).first->second;
	}else{
		serviceInfo = &itr->second;
	}

	BYTE table_id = (BYTE)eit.GetNumber(Desc::table_id);
	BYTE version_number = (BYTE)eit.GetNumber(Desc::version_number);
	BYTE section_number = (BYTE)eit.GetNumber(Desc::section_number);
	SI_TAG siTag;
	siTag.tableID = table_id;
	siTag.version = version_number;
	siTag.time = (DWORD)(streamTime / (10 * I64_1SEC));

	DWORD eventLoopSize = 0;
	Desc::CDescriptor::CLoopPointer lp;
	if( eit.EnterLoop(lp) ){
		eventLoopSize = eit.GetLoopSize(lp);
	}
	if( table_id <= 0x4F && section_number <= 1 ){
		//[p/f]
		if( siTag.time == 0 ){
			//チャンネル変更時の応答性のため、タイムスタンプ不明の[p/f]が来たらDB上の不明でない[p/f]をクリアする
			//EPGファイルを入力するときは古い[p/f]による上書きが発生するので、利用側で時系列にするかタイムスタンプを確定させる工夫が必要
			if( serviceInfo->nowEvent.empty() == false && serviceInfo->nowEvent.back().time != 0 ||
			    serviceInfo->nextEvent.empty() == false && serviceInfo->nextEvent.back().time != 0 ){
				serviceInfo->nowEvent.clear();
				serviceInfo->nextEvent.clear();
			}
		}
		if( eventLoopSize == 0 ){
			//空セクション
			if( section_number == 0 ){
				if( serviceInfo->nowEvent.empty() == false && siTag.time >= serviceInfo->nowEvent.back().time ){
					serviceInfo->nowEvent.clear();
				}
			}else{
				if( serviceInfo->nextEvent.empty() == false && siTag.time >= serviceInfo->nextEvent.back().time ){
					serviceInfo->nextEvent.clear();
				}
			}
		}
	}
	//イベントごとに更新必要が判定
	for( DWORD i = 0; i < eventLoopSize; i++ ){
		eit.SetLoopIndex(lp, i);
		WORD event_id = (WORD)eit.GetNumber(Desc::event_id, lp);
		FILETIME start_time = {};
		DWORD mjd = eit.GetNumber(Desc::start_time_mjd, lp);
		DWORD bcd = eit.GetNumber(Desc::start_time_bcd, lp);
		if( mjd != 0xFFFF || bcd != 0xFFFFFF ){
			start_time = MJDtoFILETIME(mjd, bcd);
		}
		DWORD duration = eit.GetNumber(Desc::d_duration, lp);
		if( duration != 0xFFFFFF ){
			duration = (duration >> 20 & 15) * 36000 + (duration >> 16 & 15) * 3600 + (duration >> 12 & 15) * 600 +
			           (duration >> 8 & 15) * 60 + (duration >> 4 & 15) * 10 + (duration & 15);
		}
		map<WORD, EVENT_INFO>::iterator itrEvent;
		EVENT_INFO* eventInfo = NULL;

		if( eit.GetNumber(Desc::running_status, lp) == 1 || eit.GetNumber(Desc::running_status, lp) == 3 ){
			//非実行中または停止中
			_OutputDebugString(L"★非実行中または停止中イベント ONID:0x%04x TSID:0x%04x SID:0x%04x EventID:0x%04x\r\n",
				original_network_id,  transport_stream_id, service_id, event_id
				);
			continue;
		}

#ifdef DEBUG_EIT
		sprintf_s(g_szDebugEIT, "%c%04x.%02x%02x.%02d.%d\r\n", table_id <= 0x4F ? 'P' : 'S',
			eitEventInfo->event_id, table_id, section_number, version_number, siTag.time % 1000000);
#endif
		//[actual]と[other]は等価に扱う
		//[p/f]と[schedule]は各々完全に独立してデータベースを作成する
		if( table_id <= 0x4F && section_number <= 1 ){
			//[p/f]
			if( section_number == 0 ){
				if( start_time.dwHighDateTime == 0 ){
					OutputDebugString(L"Invalid EIT[p/f]\r\n");
				}else if( serviceInfo->nowEvent.empty() || siTag.time >= serviceInfo->nowEvent.back().time ){
					if( serviceInfo->nowEvent.empty() || serviceInfo->nowEvent.back().db.event_id != event_id ){
						//イベント入れ替わり
						serviceInfo->nowEvent.clear();
						if( serviceInfo->nextEvent.empty() == false && serviceInfo->nextEvent.back().db.event_id == event_id ){
							serviceInfo->nowEvent.swap(serviceInfo->nextEvent);
							serviceInfo->nowEvent.back().time = 0;
						}
					}
					if( serviceInfo->nowEvent.empty() ){
						serviceInfo->nowEvent.resize(1);
						eventInfo = serviceInfo->nowEvent.data();
						eventInfo->db.event_id = event_id;
						eventInfo->time = 0;
						eventInfo->tagBasic.version = 0xFF;
						eventInfo->tagBasic.time = 0;
						eventInfo->tagExt.version = 0xFF;
						eventInfo->tagExt.time = 0;
					}else{
						eventInfo = serviceInfo->nowEvent.data();
					}
				}
			}else{
				if( serviceInfo->nextEvent.empty() || siTag.time >= serviceInfo->nextEvent.back().time ){
					if( serviceInfo->nextEvent.empty() || serviceInfo->nextEvent.back().db.event_id != event_id ){
						serviceInfo->nextEvent.clear();
						if( serviceInfo->nowEvent.empty() == false && serviceInfo->nowEvent.back().db.event_id == event_id ){
							serviceInfo->nextEvent.swap(serviceInfo->nowEvent);
							serviceInfo->nextEvent.back().time = 0;
						}
					}
					if( serviceInfo->nextEvent.empty() ){
						serviceInfo->nextEvent.resize(1);
						eventInfo = serviceInfo->nextEvent.data();
						eventInfo->db.event_id = event_id;
						eventInfo->time = 0;
						eventInfo->tagBasic.version = 0xFF;
						eventInfo->tagBasic.time = 0;
						eventInfo->tagExt.version = 0xFF;
						eventInfo->tagExt.time = 0;
					}else{
						eventInfo = serviceInfo->nextEvent.data();
					}
				}
			}
		}else if( PID != 0x0012 || table_id > 0x4F ){
			//[schedule]もしくは(H-EITでないとき)[p/f after]
			//TODO: イベント消滅には対応していない(クラス設計的に対応は厳しい)。EDCB的には実用上のデメリットはあまり無い
			if( start_time.dwHighDateTime == 0 || duration == 0xFFFFFF ){
				OutputDebugString(L"Invalid EIT[schedule]\r\n");
			}else{
				itrEvent = serviceInfo->eventMap.find(event_id);
				if( itrEvent == serviceInfo->eventMap.end() ){
					eventInfo = &serviceInfo->eventMap[event_id];
					eventInfo->db.event_id = event_id;
					eventInfo->time = 0;
					eventInfo->tagBasic.version = 0xFF;
					eventInfo->tagBasic.time = 0;
					eventInfo->tagExt.version = 0xFF;
					eventInfo->tagExt.time = 0;
				}else{
					eventInfo = &itrEvent->second;
				}
			}
		}
		if( eventInfo ){
			//開始時間等はタイムスタンプのみを基準に更新
			if( siTag.time >= eventInfo->time ){
				eventInfo->db.StartTimeFlag = start_time.dwHighDateTime != 0;
				SYSTEMTIME stZero = {};
				eventInfo->db.start_time = stZero;
				if( eventInfo->db.StartTimeFlag ){
					FileTimeToSystemTime(&start_time, &eventInfo->db.start_time);
				}
				eventInfo->db.DurationFlag = duration != 0xFFFFFF;
				eventInfo->db.durationSec = duration != 0xFFFFFF ? duration : 0;
				eventInfo->db.freeCAFlag = eit.GetNumber(Desc::free_CA_mode, lp) != 0;
				eventInfo->time = siTag.time;
			}
			//記述子はテーブルバージョンも加味して更新(単に効率のため)
			if( siTag.time >= eventInfo->tagExt.time ){
				if( version_number != eventInfo->tagExt.version ||
				    table_id != eventInfo->tagExt.tableID ||
				    siTag.time > eventInfo->tagExt.time + 180 ){
					if( AddExtEvent(&eventInfo->db, eit, lp) != FALSE ){
						eventInfo->tagExt = siTag;
					}
				}else{
					eventInfo->tagExt.time = siTag.time;
				}
			}
			//[schedule extended]以外
			if( (table_id < 0x58 || 0x5F < table_id) && (table_id < 0x68 || 0x6F < table_id) ){
				if( table_id > 0x4F && eventInfo->tagBasic.version != 0xFF && eventInfo->tagBasic.tableID <= 0x4F ){
					//[schedule][p/f after]とも運用するサービスがあれば[p/f after]を優先する(今のところサービス階層が分離しているのであり得ないはず)
					_OutputDebugString(L"Conflicts EIT[schedule][p/f after] SID:0x%04x EventID:0x%04x\r\n", service_id, eventInfo->db.event_id);
				}else if( siTag.time >= eventInfo->tagBasic.time ){
					if( version_number != eventInfo->tagBasic.version ||
					    table_id != eventInfo->tagBasic.tableID ||
					    siTag.time > eventInfo->tagBasic.time + 180 ){
						AddBasicInfo(&eventInfo->db, eit, lp, original_network_id, transport_stream_id);
					}
					eventInfo->tagBasic = siTag;
				}
			}
		}
	}

	//セクションステータス
	if( PID != 0x0012 ){
		//L-EIT
		if( table_id <= 0x4F ){
			if( serviceInfo->lastTableID != table_id ||
			    serviceInfo->sectionList[0].version != version_number + 1 ){
				serviceInfo->lastTableID = 0;
			}
			if( serviceInfo->lastTableID == 0 ){
				//リセット
				memset(&serviceInfo->sectionList.front(), 0, sizeof(SECTION_FLAG_INFO) * 8);
				for( int i = 1; i < 8; i++ ){
					//第0テーブル以外のセクションを無視
					memset(serviceInfo->sectionList[i].ignoreFlags, 0xFF, sizeof(serviceInfo->sectionList[0].ignoreFlags));
				}
				serviceInfo->lastTableID = table_id;
			}
			//第0セグメント以外のセクションを無視
			memset(serviceInfo->sectionList[0].ignoreFlags + 1, 0xFF, sizeof(serviceInfo->sectionList[0].ignoreFlags) - 1);
			//第0セグメントの送られないセクションを無視
			for( int i = eit.GetNumber(Desc::segment_last_section_number) % 8 + 1; i < 8; i++ ){
				serviceInfo->sectionList[0].ignoreFlags[0] |= 1 << i;
			}
			serviceInfo->sectionList[0].version = version_number + 1;
			serviceInfo->sectionList[0].flags[0] |= 1 << (section_number % 8);
		}

	}else{
		//H-EIT
		if( table_id > 0x4F ){
			BYTE& lastTableID = table_id % 16 >= 8 ? serviceInfo->lastTableIDExt : serviceInfo->lastTableID;
			vector<SECTION_FLAG_INFO>& sectionList = table_id % 16 >= 8 ? serviceInfo->sectionExtList : serviceInfo->sectionList;
			if( sectionList.empty() ){
				//拡張情報はないことも多いので遅延割り当て
				sectionList.resize(8);
			}
			if( lastTableID != eit.GetNumber(Desc::last_table_id) ){
				lastTableID = 0;
			}else if( sectionList[table_id % 8].version != 0 &&
			          sectionList[table_id % 8].version != version_number + 1 ){
				OutputDebugString(L"EIT[schedule] updated\r\n");
				lastTableID = 0;
			}
			if( lastTableID == 0 ){
				//リセット
				memset(&sectionList.front(), 0, sizeof(SECTION_FLAG_INFO) * 8);
				for( int i = eit.GetNumber(Desc::last_table_id) % 8 + 1; i < 8; i++ ){
					//送られないテーブルのセクションを無視
					memset(sectionList[i].ignoreFlags, 0xFF, sizeof(sectionList[0].ignoreFlags));
				}
				lastTableID = (BYTE)eit.GetNumber(Desc::last_table_id);
			}
			//送られないセグメントのセクションを無視
			memset(sectionList[table_id % 8].ignoreFlags + (BYTE)eit.GetNumber(Desc::last_section_number) / 8 + 1, 0xFF,
				sizeof(sectionList[0].ignoreFlags) - (BYTE)eit.GetNumber(Desc::last_section_number) / 8 - 1);
			if( table_id % 8 == 0 && streamTime > 0 ){
				//放送済みセグメントのセクションを無視
				memset(sectionList[0].ignoreFlags, 0xFF, streamTime / (3 * 60 * 60 * I64_1SEC) % 8);
			}
			//このセグメントの送られないセクションを無視
			for( int i = eit.GetNumber(Desc::segment_last_section_number) % 8 + 1; i < 8; i++ ){
				sectionList[table_id % 8].ignoreFlags[section_number / 8] |= 1 << i;
			}
			sectionList[table_id % 8].version = version_number + 1;
			sectionList[table_id % 8].flags[section_number / 8] |= 1 << (section_number % 8);
		}
	}

	return TRUE;
}

void CEpgDBUtil::AddBasicInfo(EPGDB_EVENT_INFO* eventInfo, const Desc::CDescriptor& eit, Desc::CDescriptor::CLoopPointer lpParent, WORD onid, WORD tsid)
{
	BOOL foundShort = FALSE;
	BOOL foundContent = FALSE;
	BOOL foundComponent = FALSE;
	BOOL foundGroup = FALSE;
	BOOL foundRelay = FALSE;
	Desc::CDescriptor::CLoopPointer lp = lpParent;
	if( eit.EnterLoop(lp) ){
		for( DWORD i = 0; eit.SetLoopIndex(lp, i); i++ ){
			switch( eit.GetNumber(Desc::descriptor_tag, lp) ){
			case Desc::short_event_descriptor:
				AddShortEvent(eventInfo, eit, lp);
				foundShort = TRUE;
				break;
			case Desc::content_descriptor:
				AddContent(eventInfo, eit, lp);
				foundContent = TRUE;
				break;
			case Desc::component_descriptor:
				AddComponent(eventInfo, eit, lp);
				foundComponent = TRUE;
				break;
			case Desc::event_group_descriptor:
				if( eit.GetNumber(Desc::group_type, lp) == 1 ){
					AddEventGroup(eventInfo, eit, lp, onid, tsid);
					foundGroup = TRUE;
				}else if( eit.GetNumber(Desc::group_type, lp) == 2 || eit.GetNumber(Desc::group_type, lp) == 4 ){
					AddEventRelay(eventInfo, eit, lp, onid, tsid);
					foundRelay = TRUE;
				}
				break;
			}
		}
	}
	if( AddAudioComponent(eventInfo, eit, lpParent) == FALSE ){
		eventInfo->audioInfo.reset();
	}
	if( foundShort == FALSE ){
		eventInfo->shortInfo.reset();
	}
	if( foundContent == FALSE ){
		eventInfo->contentInfo.reset();
	}
	if( foundComponent == FALSE ){
		eventInfo->componentInfo.reset();
	}
	if( foundGroup == FALSE ){
		eventInfo->eventGroupInfo.reset();
	}
	if( foundRelay == FALSE ){
		eventInfo->eventRelayInfo.reset();
	}
}

void CEpgDBUtil::AddShortEvent(EPGDB_EVENT_INFO* eventInfo, const Desc::CDescriptor& eit, Desc::CDescriptor::CLoopPointer lp)
{
	if( eventInfo->shortInfo == NULL ){
		eventInfo->shortInfo.reset(new EPGDB_SHORT_EVENT_INFO);
	}
	{
		CARIB8CharDecode arib;
		string event_name = "";
		string text_char = "";
		const BYTE* src;
		DWORD srcSize;
		src = eit.GetBinary(Desc::event_name_char, &srcSize, lp);
		if( src && srcSize > 0 ){
			arib.PSISI(src, srcSize, &event_name);
		}
		src = eit.GetBinary(Desc::text_char, &srcSize, lp);
		if( src && srcSize > 0 ){
			arib.PSISI(src, srcSize, &text_char);
		}
#ifdef DEBUG_EIT
		text_char = g_szDebugEIT + text_char;
#endif

		AtoW(event_name, eventInfo->shortInfo->event_name);
		AtoW(text_char, eventInfo->shortInfo->text_char);
	}
}

BOOL CEpgDBUtil::AddExtEvent(EPGDB_EVENT_INFO* eventInfo, const Desc::CDescriptor& eit, Desc::CDescriptor::CLoopPointer lpParent)
{
	{
		BOOL foundFlag = FALSE;
		CARIB8CharDecode arib;
		string extendText = "";
		vector<BYTE> itemBuff;
		BOOL itemDescFlag = FALSE;
		//text_lengthは0で運用される

		Desc::CDescriptor::CLoopPointer lp = lpParent;
		if( eit.EnterLoop(lp) ){
			for( DWORD i = 0; eit.SetLoopIndex(lp, i); i++ ){
				if( eit.GetNumber(Desc::descriptor_tag, lp) != Desc::extended_event_descriptor ){
					continue;
				}
				foundFlag = TRUE;
				Desc::CDescriptor::CLoopPointer lp2 = lp;
				if( eit.EnterLoop(lp2) ){
					for( DWORD j=0; eit.SetLoopIndex(lp2, j); j++ ){
						const BYTE* src;
						DWORD srcSize;
						src = eit.GetBinary(Desc::item_description_char, &srcSize, lp2);
						if( src && srcSize > 0 ){
							if( itemDescFlag == FALSE && itemBuff.size() > 0 ){
								string buff = "";
								arib.PSISI(&itemBuff.front(), (DWORD)itemBuff.size(), &buff);
								buff += "\r\n";
								extendText += buff;
								itemBuff.clear();
							}
							itemDescFlag = TRUE;
							itemBuff.insert(itemBuff.end(), src, src + srcSize);
						}
						src = eit.GetBinary(Desc::item_char, &srcSize, lp2);
						if( src && srcSize > 0 ){
							if( itemDescFlag && itemBuff.size() > 0 ){
								string buff = "";
								arib.PSISI(&itemBuff.front(), (DWORD)itemBuff.size(), &buff);
								buff += "\r\n";
								extendText += buff;
								itemBuff.clear();
							}
							itemDescFlag = FALSE;
							itemBuff.insert(itemBuff.end(), src, src + srcSize);
						}
					}
				}
			}
		}

		if( itemBuff.size() > 0 ){
			string buff = "";
			arib.PSISI(&itemBuff.front(), (DWORD)itemBuff.size(), &buff);
			buff += "\r\n";
			extendText += buff;
		}

		if( foundFlag == FALSE ){
			return FALSE;
		}
		if( eventInfo->extInfo == NULL ){
			eventInfo->extInfo.reset(new EPGDB_EXTENDED_EVENT_INFO);
		}
#ifdef DEBUG_EIT
		extendText = g_szDebugEIT + extendText;
#endif
		AtoW(extendText, eventInfo->extInfo->text_char);
	}

	return TRUE;
}

void CEpgDBUtil::AddContent(EPGDB_EVENT_INFO* eventInfo, const Desc::CDescriptor& eit, Desc::CDescriptor::CLoopPointer lp)
{
	if( eventInfo->contentInfo == NULL ){
		eventInfo->contentInfo.reset(new EPGDB_CONTEN_INFO);
	}
	{
		eventInfo->contentInfo->nibbleList.clear();
		if( eit.EnterLoop(lp) ){
			eventInfo->contentInfo->nibbleList.resize(eit.GetLoopSize(lp));
			for( DWORD i=0; eit.SetLoopIndex(lp, i); i++ ){
				EPG_CONTENT nibble;
				nibble.content_nibble_level_1 = (BYTE)eit.GetNumber(Desc::content_nibble_level_1, lp);
				nibble.content_nibble_level_2 = (BYTE)eit.GetNumber(Desc::content_nibble_level_2, lp);
				nibble.user_nibble_1 = (BYTE)eit.GetNumber(Desc::user_nibble_1, lp);
				nibble.user_nibble_2 = (BYTE)eit.GetNumber(Desc::user_nibble_2, lp);
				eventInfo->contentInfo->nibbleList[i] = nibble;
			}
		}
	}
}

void CEpgDBUtil::AddComponent(EPGDB_EVENT_INFO* eventInfo, const Desc::CDescriptor& eit, Desc::CDescriptor::CLoopPointer lp)
{
	if( eventInfo->componentInfo == NULL ){
		eventInfo->componentInfo.reset(new EPGDB_COMPONENT_INFO);
	}
	{
		eventInfo->componentInfo->stream_content = (BYTE)eit.GetNumber(Desc::stream_content, lp);
		eventInfo->componentInfo->component_type = (BYTE)eit.GetNumber(Desc::component_type, lp);
		eventInfo->componentInfo->component_tag = (BYTE)eit.GetNumber(Desc::component_tag, lp);

		CARIB8CharDecode arib;
		string text_char = "";
		DWORD srcSize;
		const BYTE* src = eit.GetBinary(Desc::text_char, &srcSize, lp);
		if( src && srcSize > 0 ){
			arib.PSISI(src, srcSize, &text_char);
		}
		AtoW(text_char, eventInfo->componentInfo->text_char);

	}
}

BOOL CEpgDBUtil::AddAudioComponent(EPGDB_EVENT_INFO* eventInfo, const Desc::CDescriptor& eit, Desc::CDescriptor::CLoopPointer lpParent)
{
	{
		WORD listSize = 0;
		Desc::CDescriptor::CLoopPointer lp = lpParent;
		if( eit.EnterLoop(lp) ){
			for( DWORD i = 0; eit.SetLoopIndex(lp, i); i++ ){
				if( eit.GetNumber(Desc::descriptor_tag, lp) == Desc::audio_component_descriptor ){
					listSize++;
				}
			}
		}
		if( listSize == 0 ){
			return FALSE;
		}
		if( eventInfo->audioInfo == NULL ){
			eventInfo->audioInfo.reset(new EPGDB_AUDIO_COMPONENT_INFO);
		}
		eventInfo->audioInfo->componentList.clear();
		eventInfo->audioInfo->componentList.resize(listSize);

		for( WORD i=0, j=0; j<eventInfo->audioInfo->componentList.size(); i++ ){
			eit.SetLoopIndex(lp, i);
			if( eit.GetNumber(Desc::descriptor_tag, lp) == Desc::audio_component_descriptor ){
				EPGDB_AUDIO_COMPONENT_INFO_DATA& item = eventInfo->audioInfo->componentList[j++];

				item.stream_content = (BYTE)eit.GetNumber(Desc::stream_content, lp);
				item.component_type = (BYTE)eit.GetNumber(Desc::component_type, lp);
				item.component_tag = (BYTE)eit.GetNumber(Desc::component_tag, lp);

				item.stream_type = (BYTE)eit.GetNumber(Desc::stream_type, lp);
				item.simulcast_group_tag = (BYTE)eit.GetNumber(Desc::simulcast_group_tag, lp);
				item.ES_multi_lingual_flag = (BYTE)eit.GetNumber(Desc::ES_multi_lingual_flag, lp);
				item.main_component_flag = (BYTE)eit.GetNumber(Desc::main_component_flag, lp);
				item.quality_indicator = (BYTE)eit.GetNumber(Desc::quality_indicator, lp);
				item.sampling_rate = (BYTE)eit.GetNumber(Desc::sampling_rate, lp);


				CARIB8CharDecode arib;
				string text_char = "";
				DWORD srcSize;
				const BYTE* src = eit.GetBinary(Desc::text_char, &srcSize, lp);
				if( src && srcSize > 0 ){
					arib.PSISI(src, srcSize, &text_char);
				}
				AtoW(text_char, item.text_char);

			}
		}
	}

	return TRUE;
}

void CEpgDBUtil::AddEventGroup(EPGDB_EVENT_INFO* eventInfo, const Desc::CDescriptor& eit, Desc::CDescriptor::CLoopPointer lp, WORD onid, WORD tsid)
{
	if( eventInfo->eventGroupInfo == NULL ){
		eventInfo->eventGroupInfo.reset(new EPGDB_EVENTGROUP_INFO);
	}
	{
		eventInfo->eventGroupInfo->group_type = (BYTE)eit.GetNumber(Desc::group_type, lp);
		eventInfo->eventGroupInfo->eventDataList.clear();
		if( eit.EnterLoop(lp) ){
			eventInfo->eventGroupInfo->eventDataList.resize(eit.GetLoopSize(lp));
			for( DWORD i=0; eit.SetLoopIndex(lp, i); i++ ){
				EPG_EVENT_DATA item;
				item.event_id = (WORD)eit.GetNumber(Desc::event_id, lp);
				item.service_id = (WORD)eit.GetNumber(Desc::service_id, lp);
				item.original_network_id = onid;
				item.transport_stream_id = tsid;

				eventInfo->eventGroupInfo->eventDataList[i] = item;
			}
		}
	}
}

void CEpgDBUtil::AddEventRelay(EPGDB_EVENT_INFO* eventInfo, const Desc::CDescriptor& eit, Desc::CDescriptor::CLoopPointer lp, WORD onid, WORD tsid)
{
	if( eventInfo->eventRelayInfo == NULL ){
		eventInfo->eventRelayInfo.reset(new EPGDB_EVENTGROUP_INFO);
	}
	{
		eventInfo->eventRelayInfo->group_type = (BYTE)eit.GetNumber(Desc::group_type, lp);
		eventInfo->eventRelayInfo->eventDataList.clear();
		if( eventInfo->eventRelayInfo->group_type == 0x02 ){
			if( eit.EnterLoop(lp) ){
				eventInfo->eventRelayInfo->eventDataList.resize(eit.GetLoopSize(lp));
				for( DWORD i=0; eit.SetLoopIndex(lp, i); i++ ){
					EPG_EVENT_DATA item;
					item.event_id = (WORD)eit.GetNumber(Desc::event_id, lp);
					item.service_id = (WORD)eit.GetNumber(Desc::service_id, lp);
					item.original_network_id = onid;
					item.transport_stream_id = tsid;

					eventInfo->eventRelayInfo->eventDataList[i] = item;
				}
			}
		}else{
			if( eit.EnterLoop(lp, 1) ){
				//他ネットワークへのリレー情報は第2ループにあるので、これは記述子のevent_countの値とは異なる
				eventInfo->eventRelayInfo->eventDataList.resize(eit.GetLoopSize(lp));
				for( DWORD i=0; eit.SetLoopIndex(lp, i); i++ ){
					EPG_EVENT_DATA item;
					item.event_id = (WORD)eit.GetNumber(Desc::event_id, lp);
					item.service_id = (WORD)eit.GetNumber(Desc::service_id, lp);
					item.original_network_id = (WORD)eit.GetNumber(Desc::original_network_id, lp);
					item.transport_stream_id = (WORD)eit.GetNumber(Desc::transport_stream_id, lp);

					eventInfo->eventRelayInfo->eventDataList[i] = item;
				}
			}
		}

	}
}

void CEpgDBUtil::ClearSectionStatus()
{
	CBlockLock lock(&this->dbLock);

	map<ULONGLONG, SERVICE_EVENT_INFO>::iterator itr;
	for( itr = this->serviceEventMap.begin(); itr != this->serviceEventMap.end(); itr++ ){
		itr->second.lastTableID = 0;
		itr->second.lastTableIDExt = 0;
	}
}

BOOL CEpgDBUtil::CheckSectionAll(const vector<SECTION_FLAG_INFO>& sectionList)
{
	for( size_t i = 0; i < sectionList.size(); i++ ){
		for( int j = 0; j < sizeof(sectionList[0].flags); j++ ){
			if( (sectionList[i].flags[j] | sectionList[i].ignoreFlags[j]) != 0xFF ){
				return FALSE;
			}
		}
	}

	return TRUE;
}

EPG_SECTION_STATUS CEpgDBUtil::GetSectionStatusService(
	WORD originalNetworkID,
	WORD transportStreamID,
	WORD serviceID,
	BOOL l_eitFlag
	)
{
	CBlockLock lock(&this->dbLock);

	map<ULONGLONG, SERVICE_EVENT_INFO>::const_iterator itr =
		this->serviceEventMap.find(_Create64Key(originalNetworkID, transportStreamID, serviceID));
	if( itr != this->serviceEventMap.end() ){
		if( l_eitFlag ){
			//L-EITの状況
			if( itr->second.lastTableID != 0 && itr->second.lastTableID <= 0x4F ){
				return CheckSectionAll(itr->second.sectionList) ? EpgLEITAll : EpgNeedData;
			}
		}else{
			//H-EITの状況
			if( itr->second.lastTableID > 0x4F ){
				//拡張情報がない場合も「たまった」とみなす
				BOOL extFlag = itr->second.lastTableIDExt == 0 || CheckSectionAll(itr->second.sectionExtList);
				return CheckSectionAll(itr->second.sectionList) ? (extFlag ? EpgHEITAll : EpgBasicAll) : (extFlag ? EpgExtendAll : EpgNeedData);
			}
		}
	}
	return EpgNoData;
}

EPG_SECTION_STATUS CEpgDBUtil::GetSectionStatus(BOOL l_eitFlag)
{
	CBlockLock lock(&this->dbLock);

	EPG_SECTION_STATUS status = EpgNoData;

	map<ULONGLONG, SERVICE_EVENT_INFO>::iterator itr;
	for( itr = this->serviceEventMap.begin(); itr != this->serviceEventMap.end(); itr++ ){
		EPG_SECTION_STATUS s = GetSectionStatusService((WORD)(itr->first >> 32), (WORD)(itr->first >> 16), (WORD)itr->first, l_eitFlag);
		if( s != EpgNoData ){
			if( status == EpgNoData ){
				status = l_eitFlag ? EpgLEITAll : EpgHEITAll;
			}
			if( l_eitFlag ){
				if( s == EpgNeedData ){
					status = EpgNeedData;
					break;
				}
			}else{
				//サービスリストあるなら映像サービスのみ対象
				map<ULONGLONG, BYTE>::iterator itrType;
				itrType = this->serviceList.find(itr->first);
				if( itrType != this->serviceList.end() ){
					if( itrType->second != 0x01 && itrType->second != 0xA5 ){
						continue;
					}
				}
				if( s == EpgNeedData || s == EpgExtendAll && status == EpgBasicAll || s == EpgBasicAll && status == EpgExtendAll ){
					status = EpgNeedData;
					break;
				}
				if( status == EpgHEITAll ){
					status = s;
				}
			}
		}
	}

	return status;
}

BOOL CEpgDBUtil::AddServiceListNIT(const Desc::CDescriptor& nit)
{
	CBlockLock lock(&this->dbLock);

	wstring network_nameW = L"";

	Desc::CDescriptor::CLoopPointer lp;
	if( nit.EnterLoop(lp) ){
		for( DWORD i = 0; nit.SetLoopIndex(lp, i); i++ ){
			if( nit.GetNumber(Desc::descriptor_tag, lp) == Desc::network_name_descriptor ){
				DWORD srcSize;
				const BYTE* src = nit.GetBinary(Desc::d_char, &srcSize, lp);
				if( src && srcSize > 0 ){
					CARIB8CharDecode arib;
					string network_name = "";
					arib.PSISI(src, srcSize, &network_name);
					AtoW(network_name, network_nameW);
				}
			}
		}
	}

	lp = Desc::CDescriptor::CLoopPointer();
	if( nit.EnterLoop(lp, 1) ){
		for( DWORD i = 0; nit.SetLoopIndex(lp, i); i++ ){
			//サービス情報更新用
			WORD onid = (WORD)nit.GetNumber(Desc::original_network_id, lp);
			WORD tsid = (WORD)nit.GetNumber(Desc::transport_stream_id, lp);
			map<DWORD, DB_TS_INFO>::iterator itrFind;
			itrFind = this->serviceInfoList.find((DWORD)onid << 16 | tsid);
			if( itrFind != this->serviceInfoList.end() ){
				itrFind->second.network_name = network_nameW;
			}

			Desc::CDescriptor::CLoopPointer lp2 = lp;
			if( nit.EnterLoop(lp2) ){
				for( DWORD j = 0; nit.SetLoopIndex(lp2, j); j++ ){
					if( nit.GetNumber(Desc::descriptor_tag, lp2) == Desc::service_list_descriptor ){
						Desc::CDescriptor::CLoopPointer lp3 = lp2;
						if( nit.EnterLoop(lp3) ){
							for( DWORD k=0; nit.SetLoopIndex(lp3, k); k++ ){
								ULONGLONG key = _Create64Key(onid, tsid, (WORD)nit.GetNumber(Desc::service_id, lp3));
								map<ULONGLONG, BYTE>::iterator itrService;
								itrService = this->serviceList.find(key);
								if( itrService == this->serviceList.end() ){
									this->serviceList.insert(pair<ULONGLONG, BYTE>(key, (BYTE)nit.GetNumber(Desc::service_type, lp3)));
								}
							}
						}
					}
					if( nit.GetNumber(Desc::descriptor_tag, lp2) == Desc::ts_information_descriptor && itrFind != this->serviceInfoList.end()){
						//ts_nameとremote_control_key_id
						DWORD srcSize;
						const BYTE* src = nit.GetBinary(Desc::ts_name_char, &srcSize, lp2);
						if( src && srcSize > 0 ){
							CARIB8CharDecode arib;
							string ts_name = "";
							arib.PSISI(src, srcSize, &ts_name);
							AtoW(ts_name, itrFind->second.ts_name);
						}
						itrFind->second.remote_control_key_id = (BYTE)nit.GetNumber(Desc::remote_control_key_id, lp2);
					}
					if( nit.GetNumber(Desc::descriptor_tag, lp2) == Desc::partial_reception_descriptor && itrFind != this->serviceInfoList.end()){
						//部分受信フラグ
						Desc::CDescriptor::CLoopPointer lp3 = lp2;
						if( nit.EnterLoop(lp3) ){
							for( DWORD k=0; nit.SetLoopIndex(lp3, k); k++ ){
								map<WORD, EPGDB_SERVICE_INFO>::iterator itrService;
								itrService = itrFind->second.serviceList.find((WORD)nit.GetNumber(Desc::service_id, lp3));
								if( itrService != itrFind->second.serviceList.end() ){
									itrService->second.partialReceptionFlag = 1;
								}
							}
						}
					}
				}
			}
		}
	}

	return TRUE;
}

BOOL CEpgDBUtil::AddServiceListSIT(WORD TSID, const Desc::CDescriptor& sit)
{
	CBlockLock lock(&this->dbLock);

	WORD ONID = 0xFFFF;
	Desc::CDescriptor::CLoopPointer lp;
	if( sit.EnterLoop(lp) ){
		for( DWORD i = 0; sit.SetLoopIndex(lp, i); i++ ){
			if( sit.GetNumber(Desc::descriptor_tag, lp) == Desc::network_identification_descriptor ){
				ONID = (WORD)sit.GetNumber(Desc::network_id, lp);
			}
		}
	}
	if(ONID == 0xFFFF){
		return FALSE;
	}

	DWORD key = ((DWORD)ONID)<<16 | TSID;
	map<DWORD, DB_TS_INFO>::iterator itrTS;
	itrTS = this->serviceInfoList.find(key);
	if( itrTS == this->serviceInfoList.end() ){
		DB_TS_INFO info;
		info.remote_control_key_id = 0;

		lp = Desc::CDescriptor::CLoopPointer();
		if( sit.EnterLoop(lp, 1) ){
			for( DWORD i = 0; sit.SetLoopIndex(lp, i); i++ ){
				EPGDB_SERVICE_INFO item;
				item.ONID = ONID;
				item.TSID = TSID;
				item.SID = (WORD)sit.GetNumber(Desc::service_id, lp);

				Desc::CDescriptor::CLoopPointer lp2 = lp;
				if( sit.EnterLoop(lp2) ){
					for( DWORD j = 0; sit.SetLoopIndex(lp2, j); j++ ){
						if( sit.GetNumber(Desc::descriptor_tag, lp2) == Desc::service_descriptor ){
							CARIB8CharDecode arib;
							string service_provider_name = "";
							string service_name = "";
							const BYTE* src;
							DWORD srcSize;
							src = sit.GetBinary(Desc::service_provider_name, &srcSize, lp2);
							if( src && srcSize > 0 ){
								arib.PSISI(src, srcSize, &service_provider_name);
							}
							src = sit.GetBinary(Desc::service_name, &srcSize, lp2);
							if( src && srcSize > 0 ){
								arib.PSISI(src, srcSize, &service_name);
							}
							AtoW(service_provider_name, item.service_provider_name);
							AtoW(service_name, item.service_name);

							item.service_type = (BYTE)sit.GetNumber(Desc::service_type, lp2);
						}
					}
				}
				info.serviceList.insert(std::make_pair(item.SID, item));
			}
		}
		this->serviceInfoList.insert(std::make_pair(key, info));
	}


	return TRUE;
}

BOOL CEpgDBUtil::AddSDT(const Desc::CDescriptor& sdt)
{
	CBlockLock lock(&this->dbLock);

	DWORD key = sdt.GetNumber(Desc::original_network_id) << 16 | sdt.GetNumber(Desc::transport_stream_id);
	map<DWORD, DB_TS_INFO>::iterator itrTS;
	itrTS = this->serviceInfoList.find(key);
	if( itrTS == this->serviceInfoList.end() ){
		DB_TS_INFO info;
		info.remote_control_key_id = 0;
		itrTS = this->serviceInfoList.insert(std::make_pair(key, info)).first;
	}

	Desc::CDescriptor::CLoopPointer lp;
	if( sdt.EnterLoop(lp) ){
		for( DWORD i = 0; sdt.SetLoopIndex(lp, i); i++ ){
			map<WORD, EPGDB_SERVICE_INFO>::iterator itrS;
			itrS = itrTS->second.serviceList.find((WORD)sdt.GetNumber(Desc::service_id, lp));
			if( itrS == itrTS->second.serviceList.end()){
				EPGDB_SERVICE_INFO item;
				item.ONID = key >> 16;
				item.TSID = key & 0xFFFF;
				item.SID = (WORD)sdt.GetNumber(Desc::service_id, lp);

				Desc::CDescriptor::CLoopPointer lp2 = lp;
				if( sdt.EnterLoop(lp2) ){
					for( DWORD j = 0; sdt.SetLoopIndex(lp2, j); j++ ){
						if( sdt.GetNumber(Desc::descriptor_tag, lp2) != Desc::service_descriptor ){
							continue;
						}
						CARIB8CharDecode arib;
						string service_provider_name = "";
						string service_name = "";
						const BYTE* src;
						DWORD srcSize;
						src = sdt.GetBinary(Desc::service_provider_name, &srcSize, lp2);
						if( src && srcSize > 0 ){
							arib.PSISI(src, srcSize, &service_provider_name);
						}
						src = sdt.GetBinary(Desc::service_name, &srcSize, lp2);
						if( src && srcSize > 0 ){
							arib.PSISI(src, srcSize, &service_name);
						}
						AtoW(service_provider_name, item.service_provider_name);
						AtoW(service_name, item.service_name);

						item.service_type = (BYTE)sdt.GetNumber(Desc::service_type, lp2);
					}
				}
				itrTS->second.serviceList.insert(std::make_pair(item.SID, item));
			}
		}
	}

	return TRUE;
}

//指定サービスの全EPG情報を取得する
// originalNetworkID		[IN]取得対象のoriginalNetworkID
// transportStreamID		[IN]取得対象のtransportStreamID
// serviceID				[IN]取得対象のServiceID
// epgInfoListSize			[OUT]epgInfoListの個数
// epgInfoList				[OUT]EPG情報のリスト（DLL内で自動的にdeleteする。次に取得を行うまで有効）
BOOL CEpgDBUtil::GetEpgInfoList(
	WORD originalNetworkID,
	WORD transportStreamID,
	WORD serviceID,
	DWORD* epgInfoListSize,
	EPG_EVENT_INFO** epgInfoList_
	)
{
	CBlockLock lock(&this->dbLock);

	ULONGLONG key = _Create64Key(originalNetworkID, transportStreamID, serviceID);

	map<ULONGLONG, SERVICE_EVENT_INFO>::iterator itr;
	itr = serviceEventMap.find(key);
	if( itr == serviceEventMap.end() ){
		return FALSE;
	}

	EPGDB_EVENT_INFO* evtPF[2] = {
		itr->second.nowEvent.empty() ? NULL : &itr->second.nowEvent.back().db,
		itr->second.nextEvent.empty() ? NULL : &itr->second.nextEvent.back().db
	};
	if( evtPF[0] == NULL || evtPF[1] && evtPF[0]->event_id > evtPF[1]->event_id ){
		std::swap(evtPF[0], evtPF[1]);
	}
	size_t listSize = itr->second.eventMap.size() +
		(evtPF[0] && itr->second.eventMap.count(evtPF[0]->event_id) == 0 ? 1 : 0) +
		(evtPF[1] && itr->second.eventMap.count(evtPF[1]->event_id) == 0 ? 1 : 0);
	if( listSize == 0 ){
		return FALSE;
	}
	*epgInfoListSize = (DWORD)listSize;
	this->epgInfoList.db.clear();

	map<WORD, EVENT_INFO>::iterator itrEvt = itr->second.eventMap.begin();
	while( evtPF[0] || itrEvt != itr->second.eventMap.end() ){
		EPGDB_EXTENDED_EVENT_INFO* extInfoSchedule = NULL;
		EPGDB_EVENT_INFO* evt;
		if( itrEvt == itr->second.eventMap.end() || evtPF[0] && evtPF[0]->event_id < itrEvt->first ){
			//[p/f]を出力
			evt = evtPF[0];
			evtPF[0] = evtPF[1];
			evtPF[1] = NULL;
		}else{
			if( evtPF[0] && evtPF[0]->event_id == itrEvt->first ){
				//両方あるときは[p/f]を優先
				evt = evtPF[0];
				evtPF[0] = evtPF[1];
				evtPF[1] = NULL;
				if( evt->extInfo == NULL && itrEvt->second.db.extInfo ){
					extInfoSchedule = itrEvt->second.db.extInfo.get();
				}
			}else{
				//[schedule]を出力
				evt = &itrEvt->second.db;
			}
			itrEvt++;
		}
		this->epgInfoList.db.resize(this->epgInfoList.db.size() + 1);
		this->epgInfoList.db.back().DeepCopy(*evt);
		if( extInfoSchedule ){
			this->epgInfoList.db.back().extInfo.reset(new EPGDB_EXTENDED_EVENT_INFO(*extInfoSchedule));
		}
	}

	this->epgInfoList.Update();
	*epgInfoList_ = this->epgInfoList.data.get();

	return TRUE;
}

//指定サービスの全EPG情報を列挙する
BOOL CEpgDBUtil::EnumEpgInfoList(
	WORD originalNetworkID,
	WORD transportStreamID,
	WORD serviceID,
	BOOL (CALLBACK *enumEpgInfoListProc)(DWORD, EPG_EVENT_INFO*, LPVOID),
	LPVOID param
	)
{
	CBlockLock lock(&this->dbLock);

	map<ULONGLONG, SERVICE_EVENT_INFO>::iterator itr =
		this->serviceEventMap.find(_Create64Key(originalNetworkID, transportStreamID, serviceID));
	if( itr == this->serviceEventMap.end() ){
		return FALSE;
	}
	EPGDB_EVENT_INFO* evtPF[2] = {
		itr->second.nowEvent.empty() ? NULL : &itr->second.nowEvent.back().db,
		itr->second.nextEvent.empty() ? NULL : &itr->second.nextEvent.back().db
	};
	if( evtPF[0] == NULL || evtPF[1] && evtPF[0]->event_id > evtPF[1]->event_id ){
		std::swap(evtPF[0], evtPF[1]);
	}
	size_t listSize = itr->second.eventMap.size() +
		(evtPF[0] && itr->second.eventMap.count(evtPF[0]->event_id) == 0 ? 1 : 0) +
		(evtPF[1] && itr->second.eventMap.count(evtPF[1]->event_id) == 0 ? 1 : 0);
	if( listSize == 0 ){
		return FALSE;
	}
	if( enumEpgInfoListProc((DWORD)listSize, NULL, param) == FALSE ){
		return TRUE;
	}

	CEpgEventInfoAdapter adapter;
	map<WORD, EVENT_INFO>::iterator itrEvt = itr->second.eventMap.begin();
	while( evtPF[0] || itrEvt != itr->second.eventMap.end() ){
		//マスターを直接参照して構築する
		EPGDB_EXTENDED_EVENT_INFO* extInfoSchedule = NULL;
		EPGDB_EVENT_INFO* evt;
		if( itrEvt == itr->second.eventMap.end() || evtPF[0] && evtPF[0]->event_id < itrEvt->first ){
			//[p/f]を出力
			evt = evtPF[0];
			evtPF[0] = evtPF[1];
			evtPF[1] = NULL;
		}else{
			if( evtPF[0] && evtPF[0]->event_id == itrEvt->first ){
				//両方あるときは[p/f]を優先
				evt = evtPF[0];
				evtPF[0] = evtPF[1];
				evtPF[1] = NULL;
				if( evt->extInfo == NULL ){
					extInfoSchedule = itrEvt->second.db.extInfo.get();
				}
			}else{
				//[schedule]を出力
				evt = &itrEvt->second.db;
			}
			itrEvt++;
		}
		EPG_EVENT_INFO data = adapter.Create(evt);
		EPG_EXTENDED_EVENT_INFO extInfo;
		if( extInfoSchedule ){
			extInfo.text_charLength = (WORD)extInfoSchedule->text_char.size();
			extInfo.text_char = extInfoSchedule->text_char.c_str();
			data.extInfo = &extInfo;
		}
		if( enumEpgInfoListProc(1, &data, param) == FALSE ){
			return TRUE;
		}
	}
	return TRUE;
}

//蓄積されたEPG情報のあるサービス一覧を取得する
//SERVICE_EXT_INFOの情報はない場合がある
//引数：
// serviceListSize			[OUT]serviceListの個数
// serviceList				[OUT]サービス情報のリスト（DLL内で自動的にdeleteする。次に取得を行うまで有効）
void CEpgDBUtil::GetServiceListEpgDB(
	DWORD* serviceListSize,
	SERVICE_INFO** serviceList_
	)
{
	CBlockLock lock(&this->dbLock);

	*serviceListSize = (DWORD)this->serviceEventMap.size();
	this->serviceDataList.reset(new SERVICE_INFO[*serviceListSize]);
	this->serviceDBList.reset(new EPGDB_SERVICE_INFO[*serviceListSize]);
	this->serviceAdapterList.reset(new CServiceInfoAdapter[*serviceListSize]);

	DWORD count = 0;
	map<ULONGLONG, SERVICE_EVENT_INFO>::iterator itr;
	for(itr = this->serviceEventMap.begin(); itr != this->serviceEventMap.end(); itr++ ){
		this->serviceDataList[count].original_network_id = (WORD)(itr->first>>32);
		this->serviceDataList[count].transport_stream_id = (WORD)((itr->first&0xFFFF0000)>>16);
		this->serviceDataList[count].service_id = (WORD)(itr->first&0xFFFF);
		this->serviceDataList[count].extInfo = NULL;

		DWORD infoKey = ((DWORD)this->serviceDataList[count].original_network_id) << 16 | this->serviceDataList[count].transport_stream_id;
		map<DWORD, DB_TS_INFO>::iterator itrInfo;
		itrInfo = this->serviceInfoList.find(infoKey);
		if( itrInfo != this->serviceInfoList.end() ){
			map<WORD, EPGDB_SERVICE_INFO>::iterator itrService;
			itrService = itrInfo->second.serviceList.find(this->serviceDataList[count].service_id);
			if( itrService != itrInfo->second.serviceList.end() ){
				this->serviceDBList[count] = itrService->second;
				this->serviceDBList[count].network_name = itrInfo->second.network_name;
				this->serviceDBList[count].ts_name = itrInfo->second.ts_name;
				this->serviceDBList[count].remote_control_key_id = itrInfo->second.remote_control_key_id;
				this->serviceDataList[count] = this->serviceAdapterList[count].Create(&this->serviceDBList[count]);
			}
		}

		count++;
	}

	*serviceList_ = this->serviceDataList.get();
}

//指定サービスの現在or次のEPG情報を取得する
//引数：
// originalNetworkID		[IN]取得対象のoriginalNetworkID
// transportStreamID		[IN]取得対象のtransportStreamID
// serviceID				[IN]取得対象のServiceID
// nextFlag					[IN]TRUE（次の番組）、FALSE（現在の番組）
// epgInfo					[OUT]EPG情報（DLL内で自動的にdeleteする。次に取得を行うまで有効）
BOOL CEpgDBUtil::GetEpgInfo(
	WORD originalNetworkID,
	WORD transportStreamID,
	WORD serviceID,
	BOOL nextFlag,
	EPG_EVENT_INFO** epgInfo_
	)
{
	CBlockLock lock(&this->dbLock);

	this->epgInfo.db.clear();

	ULONGLONG key = _Create64Key(originalNetworkID, transportStreamID, serviceID);

	map<ULONGLONG, SERVICE_EVENT_INFO>::iterator itr;
	itr = serviceEventMap.find(key);
	if( itr == serviceEventMap.end() ){
		return FALSE;
	}

	if( itr->second.nowEvent.empty() == false && nextFlag == FALSE ){
		this->epgInfo.db.resize(1);
		this->epgInfo.db.back().DeepCopy(itr->second.nowEvent.back().db);
	}else if( itr->second.nextEvent.empty() == false && nextFlag == TRUE ){
		this->epgInfo.db.resize(1);
		this->epgInfo.db.back().DeepCopy(itr->second.nextEvent.back().db);
	}
	if( this->epgInfo.db.empty() == false ){
		WORD eventID = this->epgInfo.db.back().event_id;
		if( this->epgInfo.db.back().extInfo == NULL && itr->second.eventMap.count(eventID) && itr->second.eventMap[eventID].db.extInfo ){
			this->epgInfo.db.back().extInfo.reset(new EPGDB_EXTENDED_EVENT_INFO(*itr->second.eventMap[eventID].db.extInfo));
		}
		this->epgInfo.Update();
		*epgInfo_ = this->epgInfo.data.get();
		return TRUE;
	}

	return FALSE;
}

//指定イベントのEPG情報を取得する
//引数：
// originalNetworkID		[IN]取得対象のoriginalNetworkID
// transportStreamID		[IN]取得対象のtransportStreamID
// serviceID				[IN]取得対象のServiceID
// EventID					[IN]取得対象のEventID
// pfOnlyFlag				[IN]p/fからのみ検索するかどうか
// epgInfo					[OUT]EPG情報（DLL内で自動的にdeleteする。次に取得を行うまで有効）
BOOL CEpgDBUtil::SearchEpgInfo(
	WORD originalNetworkID,
	WORD transportStreamID,
	WORD serviceID,
	WORD eventID,
	BYTE pfOnlyFlag,
	EPG_EVENT_INFO** epgInfo_
	)
{
	CBlockLock lock(&this->dbLock);

	this->searchEpgInfo.db.clear();

	ULONGLONG key = _Create64Key(originalNetworkID, transportStreamID, serviceID);

	map<ULONGLONG, SERVICE_EVENT_INFO>::iterator itr;
	itr = serviceEventMap.find(key);
	if( itr == serviceEventMap.end() ){
		return FALSE;
	}

	if( itr->second.nowEvent.empty() == false && itr->second.nowEvent.back().db.event_id == eventID ){
		this->searchEpgInfo.db.resize(1);
		this->searchEpgInfo.db.back().DeepCopy(itr->second.nowEvent.back().db);
	}else if( itr->second.nextEvent.empty() == false && itr->second.nextEvent.back().db.event_id == eventID ){
		this->searchEpgInfo.db.resize(1);
		this->searchEpgInfo.db.back().DeepCopy(itr->second.nextEvent.back().db);
	}
	if( this->searchEpgInfo.db.empty() == false ){
		if( this->searchEpgInfo.db.back().extInfo == NULL && itr->second.eventMap.count(eventID) && itr->second.eventMap[eventID].db.extInfo ){
			this->searchEpgInfo.db.back().extInfo.reset(new EPGDB_EXTENDED_EVENT_INFO(*itr->second.eventMap[eventID].db.extInfo));
		}
		this->searchEpgInfo.Update();
		*epgInfo_ = this->searchEpgInfo.data.get();
		return TRUE;
	}
	if( pfOnlyFlag == 0 ){
		map<WORD, EVENT_INFO>::iterator itrEvent;
		itrEvent = itr->second.eventMap.find(eventID);
		if( itrEvent != itr->second.eventMap.end() ){
			this->searchEpgInfo.db.resize(1);
			this->searchEpgInfo.db.back().DeepCopy(itrEvent->second.db);
			this->searchEpgInfo.Update();
			*epgInfo_ = this->searchEpgInfo.data.get();
			return TRUE;
		}
	}

	return FALSE;
}

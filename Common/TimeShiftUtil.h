#pragma once
#include "../BonCtrl/PacketInit.h"
#include "../BonCtrl/PMTUtil.h"
#include "../BonCtrl/SendUDP.h"
#include "../BonCtrl/SendTCP.h"
#include "../BonCtrl/CreatePATPacket.h"
#include "TSPacketUtil.h"

class CTimeShiftUtil
{
public:
	CTimeShiftUtil(void);
	~CTimeShiftUtil(void);

	//UDP/TCP���M���s��
	//�߂�l�F
	// TRUE�i�����j�AFALSE�i���s�j
	//�����F
	// val		[IN/OUT]���M����
	BOOL Send(
		NWPLAY_PLAY_INFO* val
		);

	//�^�C���V�t�g�p�t�@�C�����J��
	//�߂�l�F
	// TRUE�i�����j�AFALSE�i���s�j
	//�����F
	// filePath		[IN]�^�C���V�t�g�p�o�b�t�@�t�@�C���̃p�X
	// fileSize		[IN]�L���ȃt�@�C���T�C�Y�B-1�Ńt�@�C���T�C�Y���̂܂܂��L���B
	// fileMode		[IN]�^��ς݃t�@�C���Đ����[�h
	BOOL OpenTimeShift(
		LPCWSTR filePath_,
		__int64 fileSize,
		BOOL fileMode_
		);

	//�^�C���V�t�g���M���J�n����
	//�߂�l�F
	// TRUE�i�����j�AFALSE�i���s�j
	BOOL StartTimeShift();

	//�^�C���V�t�g���M���~����
	//�߂�l�F
	// TRUE�i�����j�AFALSE�i���s�j
	BOOL StopTimeShift();

	//���ݗL���ȃt�@�C���T�C�Y��ݒ肷��
	//�����F
	// fileSize		[IN]�L���ȃt�@�C���T�C�Y�B-1�Ńt�@�C���T�C�Y���̂܂܂��L���B
	void SetAvailableSize(__int64 fileSize);

	//���݂̑��M�ʒu�ƗL���ȃt�@�C���T�C�Y���擾����
	//�����F
	// filePos		[OUT]�t�@�C���ʒu
	// fileSize		[OUT]�t�@�C���T�C�Y
	void GetFilePos(__int64* filePos, __int64* fileSize);

	//���M�J�n�ʒu��ύX����
	//�߂�l�F
	// TRUE�i�����j�AFALSE�i���s�j
	//�����F
	// filePos		[IN]�t�@�C���ʒu
	BOOL SetFilePos(__int64 filePos);

protected:
	HANDLE lockEvent;
	HANDLE lockBuffEvent;
	CSendUDP sendUdp;
	CSendTCP sendTcp;
	wstring sendUdpIP;
	wstring sendTcpIP;
	DWORD sendUdpPort;
	DWORD sendTcpPort;
	HANDLE udpPortMutex;
	HANDLE tcpPortMutex;

	wstring filePath;
	WORD PCR_PID;

	BOOL fileMode;
	__int64 availableFileSize;
	__int64 currentFilePos;
	__int64 totalFileSize;

	HANDLE readThread;
	HANDLE readStopEvent;

	map<WORD, CPMTUtil*> pmtUtilMap; //�L�[PMT��PID
protected:
	//PublicAPI�r������p
	BOOL Lock(LPCWSTR log = NULL, DWORD timeOut = 5*1000);
	void UnLock(LPCWSTR log = NULL);
	BOOL LockBuff(LPCWSTR log = NULL, DWORD timeOut = 5*1000);
	void UnLockBuff(LPCWSTR log = NULL);

	static UINT WINAPI ReadThread(LPVOID param);
};


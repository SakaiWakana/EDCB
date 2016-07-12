#include "StdAfx.h"
#include "FileStreamingUtil.h"
#include <process.h>

CFileStreamingUtil::CFileStreamingUtil(void)
{
}


CFileStreamingUtil::~CFileStreamingUtil(void)
{
}

BOOL CFileStreamingUtil::OpenFile(LPCWSTR filePath)
{
	return this->timeShiftUtil.OpenTimeShift(filePath, -1, TRUE);
}

BOOL CFileStreamingUtil::OpenTimeShift(LPCWSTR filePath, DWORD processID,DWORD exeCtrlID)
{
	//TODO: ������filePath�̗L���t�@�C���T�C�Y�𒲂ׂ�fileSize�Ɋi�[����
	BOOL ret = FALSE;
	__int64 fileSize = 0;
	{
		ret = this->timeShiftUtil.OpenTimeShift(filePath, fileSize, FALSE);
	}

	return ret;
}

BOOL CFileStreamingUtil::StartSend()
{
	return this->timeShiftUtil.StartTimeShift();
}

BOOL CFileStreamingUtil::StopSend()
{
	return this->timeShiftUtil.StopTimeShift();
}

//�X�g���[���z�M�Ō��݂̑��M�ʒu�Ƒ��t�@�C���T�C�Y���擾����
//�߂�l�F
// �G���[�R�[�h
//�����F
// val				[IN/OUT]�T�C�Y���
BOOL CFileStreamingUtil::GetPos(
	NWPLAY_POS_CMD* val
	)
{
	this->timeShiftUtil.GetFilePos(&val->currentPos, &val->totalPos);
	return TRUE;
}

//�X�g���[���z�M�ő��M�ʒu���V�[�N����
//�߂�l�F
// �G���[�R�[�h
//�����F
// val				[IN]�T�C�Y���
BOOL CFileStreamingUtil::SetPos(
	NWPLAY_POS_CMD* val
	)
{
	this->timeShiftUtil.SetFilePos(val->currentPos);
	return TRUE;
}

//�X�g���[���z�M�ő��M���ݒ肷��
//�߂�l�F
// �G���[�R�[�h
//�����F
// val				[IN]�T�C�Y���
BOOL CFileStreamingUtil::SetIP(
	NWPLAY_PLAY_INFO* val
	)
{
	return this->timeShiftUtil.Send(val);
}
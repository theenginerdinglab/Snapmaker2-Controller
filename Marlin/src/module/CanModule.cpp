#include "../inc/MarlinConfig.h"

#if ENABLED(CANBUS_SUPPORT)

#include "../../HAL/HAL_GD32F1/HAL_can_STM32F1.h"
#include "../Marlin.h"
#include "temperature.h"
#include "configuration_store.h"
#include "ExecuterManager.h"
#include "Periphdevice.h"
#include "CanModule.h"
#include "../SnapScreen/Screen.h"

CanModule CanModules;

#define CANID_IN  (1<<29)
#define CANID_ADDRESSBIT  (1<<28)
#define CANID_BROCAST (1)

#define MODULE_MASK_BITS  0xff80000
#define MODULE_EXECUTER_PRINT 0
#define MODULE_EXECUTER_CNC 1
#define MODULE_EXECUTER_LASER 2
#define MODULE_LINEAR 3
#define MODULE_ROTATE 4
#define MODULE_ENCLOSER 5
#define MODULE_LIGHT 6
#define MODULE_AIRCONDITIONER 7

enum
{
  CMD_T_CONFIG = 0,
  CMD_R_CONFIG_REACK,
  CMD_T_REQUEST_FUNCID,
  CMD_R_REPORT_FUNCID,
  CMD_T_CONFIG_FUNCID,
  CMD_R_CONFIG_FUNCID_REACK,
  CMD_T_UPDATE_REQUEST, //6
  CMD_R_UPDATE_REQUEST_REACK,
  CMD_T_UPDATE_PACKDATA,
  CMD_R_UPDATE_PACK_REQUEST,
  CMD_T_UPDATE_END,
};

#define MAKE_ID(MID)  ((MID << 19) & MODULE_MASK_BITS)

#define FLASH_CAN_TABLE_ADDR  (0x8000000 + 32 * 1024)

/**
 *Init:Initialize module table
 */
void CanModule::Init(void) {
  CollectPlugModules();
  PrepareLinearModules();
  PrepareExecuterModules();
}

/**
 *CollectPlugModules:Collect the IDs of the pluged modules
 */
void CanModule::CollectPlugModules() {
  uint32_t tmptick;
  uint32_t ID;
  int i;
  uint32_t ExeIDs[3];

  while(1) {
    if(CanSendPacked(CANID_BROCAST, IDTYPE_EXTID, 2, FRAME_REMOTE, 0, 0) == true) {
      tmptick = millis() + 500;
      while(tmptick > millis()) {
        ;
      }
      break;
    } else {
      SERIAL_ECHOLN("Send Error");
      break;
    }
  }

  LinearModuleCount = 0;
  for(i=0;i<CanBusControlor.ModuleCount;i++) {
    if((CanBusControlor.ModuleMacList[i] & MODULE_MASK_BITS) == MAKE_ID(MODULE_LINEAR))
      LinearModuleID[LinearModuleCount++] = CanBusControlor.ModuleMacList[i];
  }

  ExecuterCount = 0;
  ExeIDs[0] = MAKE_ID(MODULE_EXECUTER_PRINT);
  ExeIDs[1] = MAKE_ID(MODULE_EXECUTER_CNC);
  ExeIDs[2] = MAKE_ID(MODULE_EXECUTER_LASER);
  for(i=0;i<CanBusControlor.ModuleCount;i++)
  {
    ID = (CanBusControlor.ModuleMacList[i] & MODULE_MASK_BITS);
    if((ID == ExeIDs[0]) || (ID == ExeIDs[1]) || (ID == ExeIDs[2]))
      ExecuterID[ExecuterCount++] = CanBusControlor.ModuleMacList[i];
  }
}

/**
 *PrepareLinearModules:Prepare for LinearModule
 */
void CanModule::PrepareLinearModules(void) {
  uint32_t tmptick;
  uint32_t i;
  uint32_t j;
  uint16_t FuncID[9];
  uint8_t LinearAxisMark[9];
  uint8_t CanNum = 2;
  uint8_t Buff[3] = {CMD_T_CONFIG, 0x00, 0x00};
  int Pins[3] = {X_DIR_PIN, Y_DIR_PIN, Z_DIR_PIN};
  
  WRITE(X_DIR_PIN, LOW);
  WRITE(Y_DIR_PIN, LOW);
  WRITE(Z_DIR_PIN, LOW);
  
  for(i=0;i<LinearModuleCount;i++) {
    for(j=0;j<3;j++) {
      Buff[1] = j;
      Buff[2] = (j==2)?1:0;
      WRITE(Pins[j], HIGH);
      CanBusControlor.SendLongData(CanNum, LinearModuleID[i], Buff, 3);
      tmptick = millis() + 10;
      while(tmptick > millis());
      WRITE(Pins[j], LOW);
    }
    tmptick = millis() + 100;
    while(tmptick > millis()) {
      if(CanBusControlor.ProcessLongPacks(RecvBuff, 5) == 5) {
        if(RecvBuff[0] == CMD_R_CONFIG_FUNCID_REACK) {
          LinearAxisMark[i] = RecvBuff[1];
          //Axis Endstop describe,etc 0-2:xyz 3:probe min 4-6:xyz max
          LinearModuleAxis[i] = RecvBuff[1] + 4;
          LinearModuleLength[i] = (RecvBuff[2] << 8) | RecvBuff[3];
        }
        break;
      }
    }
  }

  //Get Linear module function ID
  for(i=0;i<LinearModuleCount;i++) {
    SendBuff[0] = CMD_T_REQUEST_FUNCID;
    CanBusControlor.SendLongData(CanNum, LinearModuleID[i], SendBuff, 1);
    tmptick = millis() + 100;
    FuncID[i] = 0xffff;
    while(tmptick > millis()) {
      if(CanBusControlor.ProcessLongPacks(RecvBuff, 4) == 4) {
        if(RecvBuff[0] == CMD_R_REPORT_FUNCID) {
          FuncID[i] = (RecvBuff[2] << 8) | RecvBuff[3];
        }
        break;
      }
    }
  }
  
  //Bind Function ID with the message ID
  uint8_t AxisLinearCount[3] = {0, 0, 0};
  SendBuff[0] = CMD_T_CONFIG_FUNCID;
  SendBuff[1] = 0x01;
  for(i=0;i<LinearModuleCount;i++) {
    if((AxisLinearCount[LinearAxisMark[i]] < 3) && (FuncID[i] == 0x0000)) {
      SendBuff[2] = 0;
      SendBuff[3] = LinearAxisMark[i] + 3 + AxisLinearCount[LinearAxisMark[i]] * 6;
      SendBuff[4] = (uint8_t)(FuncID[i] >> 8);
      SendBuff[5] = (uint8_t)(FuncID[i]);
      AxisLinearCount[LinearAxisMark[i]]++;
      LinearModuleMsgID[i] = SendBuff[3];
      CanBusControlor.SendLongData(CanNum, LinearModuleID[i], SendBuff, 6);

      SERIAL_ECHOLN(LinearAxisMark[i]);
    }
  }
}

/**
 *PrepareExecuterModules:Prepare for Executer module
 */
void CanModule::PrepareExecuterModules(void) {
  uint32_t tmptick;
  uint32_t i;
  uint32_t j;
  uint8_t RecvBuff[3];
  uint8_t ExecuterMark[6];
  uint8_t Buff[3] = {CMD_T_CONFIG, 0x00, 0x00};
  int32_t Pins[] = {E0_DIR_PIN};
 
  WRITE(E0_DIR_PIN, LOW);

  for(i=0;i<ExecuterCount;i++) {
    for(j=0;j<sizeof(Pins) / sizeof(Pins[0]);j++) {
      WRITE(Pins[j], HIGH);
      CanBusControlor.SendLongData(2, ExecuterID[i], Buff, 3);
      WRITE(Pins[j], LOW);
    }
    tmptick = millis() + 100;
    ExecuterMark[i] = 0xff;
    while(tmptick > millis())
    {
      if(CanBusControlor.ProcessLongPacks(RecvBuff, 3) == 3)
      {
        if(RecvBuff[0] == CMD_R_CONFIG_REACK) ExecuterMark[i] = RecvBuff[1];
        break;
      }
    }
  }

  for(j=0;j<sizeof(ExecuterMark) / sizeof(ExecuterMark[0]);j++) {
    if(((ExecuterID[j] & MODULE_MASK_BITS) == MAKE_ID(MODULE_EXECUTER_PRINT)) && (ExecuterMark[j] != 0xff)) ExecuterHead.MachineType = MACHINE_TYPE_3DPRINT;
    else if(((ExecuterID[j] & MODULE_MASK_BITS) == MAKE_ID(MODULE_EXECUTER_CNC)) && (ExecuterMark[j] != 0xff)) ExecuterHead.MachineType = MACHINE_TYPE_CNC;
    else if(((ExecuterID[j] & MODULE_MASK_BITS) == MAKE_ID(MODULE_EXECUTER_LASER)) && (ExecuterMark[j] != 0xff)) ExecuterHead.MachineType = MACHINE_TYPE_LASER;
  }
}

/**
 *LoadUpdateData:Load update data from flash
 *para Packindex:
 *para pData:The point to the buff 
 */
bool CanModule::LoadUpdatePack(uint16_t Packindex, uint8_t *pData) {
  uint32_t Address;
  uint32_t Size;
  int i;
  uint16_t Packs;
  uint16_t PackSize = 128;
  Address = FLASH_UPDATE_CONTENT;
  Size = 0;
  for(i=0;i<4;i++) {
    Size = (Size << 8) | *((uint8_t*)Address++);
  }
  Packs = (Size - 512) / 128;
  if(Size % PackSize) Packs++;
  if(Packindex >= Packs) return false;
  Address = FLASH_UPDATE_CONTENT + 512 + Packindex * PackSize;
  for(i=0;i<PackSize;i++) *pData++ = *((uint8_t*)Address++);
  return true;
}

/**
 *LoadUpdateInfo:Load update data from flash
 *para Version:Update file version
 *para StartID:
 *prar EndID:how many id type can be update when use the 
 *
 */
bool CanModule::LoadUpdateInfo(char *Version, uint16_t *StartID, uint16_t *EndID) {
  uint32_t Address;
  uint8_t Buff[33];
  Address = FLASH_UPDATE_CONTENT;
  for(int i=0;i<9;i++)
    Buff[i] = *((uint8_t*)Address++);
  if(Buff[0] != 1) return false;
  *StartID = (uint16_t)((Buff[1] << 8) | Buff[2]);
  *EndID = (uint16_t)((Buff[3] << 8) | Buff[4]);
  for(int i=0;i<33;i++)
    Version[i] = *((uint8_t*)Address++);
  return true;
}

/**
 *Update:Send Start update process
 *para CanBum:Can port number
 *para ID:Module ID
 *para Version:Update file version
 *return :true if update success, or else false
 */
bool CanModule::UpdateModule(uint8_t CanNum, uint32_t ID, char *Version) {
  uint32_t tmptick;
  uint16_t PackIndex;
  int i;
  int j;
  int err;

  //Step1:send update version
  i = 0;
  err = 1;
  SendBuff[i++] = CMD_T_UPDATE_REQUEST;
  for(j=0;(j<64) && (Version[j]!=0);j++) SendBuff[i++] = Version[j];
  CanBusControlor.SendLongData(CanNum, ID, SendBuff, 3);
  tmptick = millis() + 100;
  while(tmptick > millis()) {
    if(CanBusControlor.ProcessLongPacks(RecvBuff, 2) == 2) {    
      if(RecvBuff[0] == CMD_R_UPDATE_REQUEST_REACK)
      {
        if(RecvBuff[1] == 0x00) {
          return true;
        }
        else if(RecvBuff[1] == 0x01) {
          err = 0;
          break;
        }
          
      }
    }
  }
  if(err) {
    SERIAL_ECHOLNPAIR("Module Update Fail:" , ID);
    return false;
  }

  //Step2:send update content
  //Delay for waiting the module reboot
  tmptick = millis() + 100;
  while(tmptick > millis());
  tmptick = millis() + 1000;
  while(tmptick > millis()) {
    if(CanBusControlor.ProcessLongPacks(RecvBuff, 2) == 2) {
      if(RecvBuff[0] == CMD_R_UPDATE_PACK_REQUEST) {
        PackIndex = (uint16_t)((RecvBuff[0] << 8) | RecvBuff[1]);
        if(LoadUpdatePack(PackIndex, &SendBuff[2]) == true) {
          SendBuff[0] = CMD_T_UPDATE_PACKDATA;
          CanBusControlor.SendLongData(CanNum, ID, SendBuff, 128 + 2);
        } else {
          SendBuff[0] = CMD_T_UPDATE_END;
          CanBusControlor.SendLongData(CanNum, ID, SendBuff, 1);
          break;
        }
        tmptick = millis() + 200;
      }
    }
  }
  
  SERIAL_ECHOLNPAIR("Module Update Complete:" , ID);

  return true;
}

/**
 *UpdateProcess:
 */
void CanModule::UpdateProcess(void)
{
  int i;
  uint16_t StartID, EndID, CurTypeID;
  uint8_t CanNum;

  CanNum = 0;
  char Version[64];
  //Load Update infomation and check if it is the module update file
  if(LoadUpdateInfo(Version, &StartID, &EndID) == true) {
    SERIAL_ECHOLNPAIR("Version:", Version, "StartID", StartID, "EndID:", EndID);
    //Ergodic all modules which are suitable for updating
    for(i=0;i<CanBusControlor.ModuleCount;i++) {
      CurTypeID = CanBusControlor.ModuleMacList[i] & MODULE_MASK_BITS;
      if((CurTypeID >= StartID) && (CurTypeID <= EndID)) {
        UpdateModule(CanNum, CanBusControlor.ModuleMacList[i], Version);
      } else {
        
      }
    }
    HMI.SendUpdateComplete(1);
  }
}

/**
 *UpdateEndstops:Update endstop from can
 */
int CanModule::UpdateEndstops(uint8_t *pBuff) {
  uint16_t MsgID;
  uint16_t index;
  
  MsgID = (uint16_t)((pBuff[0] << 8) | pBuff[1]);
  for(int i=0;i<sizeof(LinearModuleMsgID) / sizeof(LinearModuleMsgID[0]);i++) {
    if(LinearModuleMsgID[i] == MsgID) {
      index = LinearModuleAxis[i];
      Endstop |= (1 << index);
      if(pBuff[2] == 0) Endstop &= ~(1 << index);
      break;
    }
  }
  return 0;
}

/**
 *SetExecuterFan1:Set executer fan1
 */
int CanModule::SetExecuterFan1(uint8_t Speed) {
  return 0;
}

/**
 *SetExecuterFan1:Set executer fan1
 */
int CanModule::SetExecuterFan2(uint8_t Speed) {
  return 0;
}

/**
 *SetExecuterFan1:Set executer fan1
 */
int CanModule::SetExecuterTemperature(uint16_t TargetTemperature) {
  return 0;
}


#endif // ENABLED CANBUS_SUPPORT
/*
 *  Copyright (C) 2010-2013, Moe Wheatley
 *  Copyright (C) 2015-2020, Alwin Esch (Team KODI)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "RDSProcess.h"

#include "FmDecode.h"

const int BLK_OFFSET_TBL[8] = {OFFSET_SYNDROME_BLOCK_A, OFFSET_SYNDROME_BLOCK_B,
                               OFFSET_SYNDROME_BLOCK_C, OFFSET_SYNDROME_BLOCK_D,
                               //
                               OFFSET_SYNDROME_BLOCK_A, OFFSET_SYNDROME_BLOCK_B,
                               OFFSET_SYNDROME_BLOCK_CP, OFFSET_SYNDROME_BLOCK_D};

/*!
 * Parity check matrix from RDS spec for FEC
 * Takes into account that the (26,16)code is a shortened cyclic block code
 * from the original (341,331) length cyclic code.
 */
const uint32_t PARCKH[16] = {
    0x2DC, // 10 1101 1100
    0x16E, // 01 0110 1110
    0x0B7, // 00 1011 0111
    0x287, // 10 1000 0111
    0x39F, // 11 1001 1111
    0x313, // 11 0001 0011
    0x355, // 11 0101 0101
    0x376, // 11 0111 0110
    0x1BB, // 01 1011 1011
    0x201, // 10 0000 0001
    0x3DC, // 11 1101 1100
    0x1EE, // 01 1110 1110
    0x0F7, // 00 1111 0111
    0x2A7, // 10 1010 0111
    0x38F, // 11 1000 1111
    0x31B // 11 0001 1011
};

cRDSRxSignalProcessor::cRDSRxSignalProcessor(cRadioReceiver* proc, RealType SampleRate)
  : m_SampleRate(SampleRate), m_Decoder(proc)
{
  m_ProcessRate = m_DownConvert.SetDataRate(m_SampleRate, 8000.0);
  m_DownConvert.SetFrequency(
      -RDS_FREQUENCY); //set up to shift 57KHz RDS down to baseband and decimate

  RealType downConvertFactor = m_SampleRate / m_ProcessRate;
  m_ProcessArrayIn = (ComplexType*)calloc(MAX_RDS_ARRAY_SIZE, sizeof(ComplexType));
  m_RdsRaw = (ComplexType*)calloc(MAX_RDS_ARRAY_SIZE / downConvertFactor, sizeof(ComplexType));
  m_RdsMag = (RealType*)calloc(MAX_RDS_ARRAY_SIZE / downConvertFactor, sizeof(RealType));
  m_RdsData = (RealType*)calloc(MAX_RDS_ARRAY_SIZE / downConvertFactor, sizeof(RealType));

  RealType norm = K_2PI / m_ProcessRate; //to normalize Hz to radians
  //initialize the PLL that is used to de-rotate the rds DSB signal
  m_RdsNcoLLimit = (m_RdsNcoFreq - RDSPLL_RANGE) * norm; //clamp RDS PLL NCO
  m_RdsNcoHLimit = (m_RdsNcoFreq + RDSPLL_RANGE) * norm;
  m_RdsPllAlpha = 2.0 * RDSPLL_ZETA * RDSPLL_BW * norm;
  m_RdsPllBeta = (m_RdsPllAlpha * m_RdsPllAlpha) / (4.0 * RDSPLL_ZETA * RDSPLL_ZETA);
  //create matched filter to extract bi-phase bits
  // This is basically the time domain shape of a single bi-phase bit
  // as defined for RDS and is close to a single cycle sine wave in shape
  m_RdsMatchCoefLength = m_ProcessRate / RDS_BITRATE;
  m_RdsMatchCoef = (RealType*)calloc(m_RdsMatchCoefLength * 2 + 1, sizeof(RealType));
  for (int i = 0; i <= m_RdsMatchCoefLength; i++)
  {
    RealType t = (RealType)i / (m_ProcessRate);
    RealType x = t * RDS_BITRATE;
    RealType x64 = 64.0 * x;
    m_RdsMatchCoef[i + m_RdsMatchCoefLength] =
        .75 * MCOS(2.0 * K_2PI * x) * ((1.0 / (1.0 / x - x64)) - (1.0 / (9.0 / x - x64)));
    m_RdsMatchCoef[m_RdsMatchCoefLength - i] =
        -.75 * MCOS(2.0 * K_2PI * x) * ((1.0 / (1.0 / x - x64)) - (1.0 / (9.0 / x - x64)));
  }
  m_RdsMatchCoefLength *= 2;

  //! initialize a bunch of variables pertaining to the rds decoder
  Reset();
}

cRDSRxSignalProcessor::~cRDSRxSignalProcessor()
{
  free(m_ProcessArrayIn);
  free(m_RdsRaw);
  free(m_RdsMag);
  free(m_RdsData);
  free(m_RdsMatchCoef);
}

void cRDSRxSignalProcessor::Reset()
{
  m_Decoder.Reset();

  m_RdsNcoPhase = 0.0;
  m_RdsNcoFreq = 0.0; //freq offset to bring to baseband

  m_RdsLPFilter.InitLPFilter(0, 1.0, 40.0, 2400.0, 1.3 * 2400.0, m_ProcessRate);

  //! load the matched filter coef into FIR filter
  m_RdsMatchedFilter.InitConstFir(m_RdsMatchCoefLength, m_RdsMatchCoef, m_ProcessRate);

  //! create Hi-Q resonator at the bit rate to recover bit sync position Q==500
  m_RdsBitSyncFilter.Init(ftBP, RDS_BITRATE, 500, m_ProcessRate);

  m_RdsLastSync = 0.0;
  m_RdsLastSyncSlope = 0.0;
  m_RdsLastBit = 0;
  m_CurrentBitPosition = 0;
  m_CurrentBlock = BLOCK__A;
  m_DecodeState = STATE_BITSYNC;
  m_BGroupOffset = 0;
  m_ClockInput = false;
  m_DataInputPrev = 0;
  m_ClockLastValue = 0.0;
  m_RdsLastData = 0.0;
}

void cRDSRxSignalProcessor::Process(const RealType* inputStream, unsigned int inLength)
{
  for (unsigned int i = 0; i < inLength; i++)
    m_ProcessArrayIn[i] = inputStream[i]; //* 2.0;//ComplexType(0, inputStream[i] /** 2.0*/);

  //! translate 57KHz RDS signal to baseband and decimate RDS complex signal
  unsigned int length = m_DownConvert.ProcessData(inLength, m_ProcessArrayIn, m_RdsRaw);

  m_RdsLPFilter.Process(m_RdsRaw, length);

  //! PLL to remove any rotation since may not be phase locked to 19KHz Pilot or may not even have pilot
  ProcessRdsPll(m_RdsRaw, m_RdsData, length);

  //! run matched filter correlator to extract the bi-phase data bits
  m_RdsMatchedFilter.Process(m_RdsData, length);

  //! create bit sync signal in m_RdsMag[] by squaring data
  for (unsigned int i = 0; i < length; i++)
    m_RdsMag[i] =
        m_RdsData[i] * m_RdsData[i]; //has high energy at the bit clock rate and 2x bit rate

  //! run Hi-Q resonator filter that create a sin wave that will lock to BitRate clock and not 2X rate
  m_RdsBitSyncFilter.Process(m_RdsMag, length);

  for (unsigned int i = 0; i < length; i++)
  {
    RealType Data = m_RdsData[i];
    RealType SyncVal = m_RdsMag[i];

    //the best bit sync position is at the positive peak of the sync sine wave
    RealType Slope = SyncVal - m_RdsLastSync; //current slope
    m_RdsLastSync = SyncVal;

    //see if at the top of the sine wave
    if ((Slope < 0.0) && (m_RdsLastSyncSlope * Slope) < 0.0)
    { //are at sample time so read previous bit time since we are one sample behind in sync position
      int bit;
      if (m_RdsLastData >= 0)
      {
        bit = 1;
        m_RdsRaw[i].real(m_RdsLastData);
      }
      else
      {
        bit = 0;
        m_RdsRaw[i].real(m_RdsLastData);
      }
      //need to XOR with previous bit to get actual data bit value
      ProcessNewRdsBit(bit ^ m_RdsLastBit); //go process new RDS Bit
      m_RdsLastBit = bit;
    }
    else
    {
      m_RdsRaw[i].real(0);
    }

    m_RdsLastData = Data; //keep last bit since is differential data
    m_RdsLastSyncSlope = Slope;
    m_RdsRaw[i].imag(Data);
  }
}

/*!
 * Less acurate but somewhat faster atan2() function
 * |error| < 0.005
 * Useful for plls but not for main FM demod if best audio quality desired.
 */
inline RealType arctan2(RealType y, RealType x)
{
  if (x == 0.0)
  { //! avoid divide by zero and just return angle
    if (y > 0.0)
      return K_PI2;
    if (y == 0.0)
      return 0.0;
    return -K_PI2;
  }

  RealType angle;
  RealType z = y / x;
  if (MFABS(z) < 1.0)
  {
    angle = z / (1.0 + 0.2854 * z * z);
    if (x < 0.0)
    {
      if (y < 0.0)
        return angle - K_PI;
      return angle + K_PI;
    }
  }
  else
  {
    angle = K_PI2 - z / (z * z + 0.2854);
    if (y < 0.0)
      return angle - K_PI;
  }
  return angle;
}

/*!
 * Process I/Q RDS baseband stream to lock PLL
 */
void cRDSRxSignalProcessor::ProcessRdsPll(ComplexType* pInData,
                                          RealType* pOutData,
                                          unsigned int InLength)
{
  RealType Sin;
  RealType Cos;
  ComplexType tmp;

  for (int i = 0; i < InLength; i++)
  {
#if TARGET_WINDOWS
    RealType* pdCosAns = &Cos;
    RealType* pdSinAns = &Sin;
    _asm
    {
      fld QWORD PTR [m_RdsNcoPhase]
      fsincos
      mov ebx,[pdCosAns]    ;  get the pointer into ebx
      fstp QWORD PTR [ebx]  ;  store the result through the pointer
      mov ebx,[pdSinAns]
      fstp QWORD PTR [ebx]
    }
#elif (defined(__i386__) || defined(__x86_64__))
    asm volatile("fsincos" : "=%&t"(Cos), "=%&u"(Sin) : "0"(m_RdsNcoPhase)); //126nS
#elif defined(__arm__)
    sincos_LP(m_RdsNcoPhase, Sin, Cos);
#else
    Sin = MSIN(m_RdsNcoPhase); //178ns for sin/cos calc
    Cos = MCOS(m_RdsNcoPhase);
#endif
    //complex multiply input sample by NCO's  sin and cos
    tmp = ComplexType(Cos * pInData[i].real() - Sin * pInData[i].imag(),
                      Cos * pInData[i].imag() + Sin * pInData[i].real());
    //find current sample phase after being shifted by NCO frequency
    RealType phzerror = -arctan2(tmp.imag(), tmp.real());
    //create new NCO frequency term
    m_RdsNcoFreq += (m_RdsPllBeta * phzerror); //  radians per sampletime
    //clamp NCO frequency so doesn't get out of lock range
    if (m_RdsNcoFreq > m_RdsNcoHLimit)
      m_RdsNcoFreq = m_RdsNcoHLimit;
    else if (m_RdsNcoFreq < m_RdsNcoLLimit)
      m_RdsNcoFreq = m_RdsNcoLLimit;
    //update NCO phase with new value
    m_RdsNcoPhase += (m_RdsNcoFreq + m_RdsPllAlpha * phzerror);
    pOutData[i] = tmp.imag();
    ;
  }
  m_RdsNcoPhase = MFMOD(m_RdsNcoPhase, K_2PI); //keep radian counter bounded
}

void cRDSRxSignalProcessor::ProcessNewRdsBit(int bit)
{
  m_InBitStream = (m_InBitStream << 1) | bit; //shift in new bit
  switch (m_DecodeState)
  {
    case STATE_BITSYNC: //looking at each bit position till we find a "good" block A
      if (!CheckBlock(OFFSET_SYNDROME_BLOCK_A, false))
      { //got initial good chkword on Block A not using FEC
        m_CurrentBitPosition = 0;
        m_BGroupOffset = 0;
        m_BlockData[BLOCK__A] = m_InBitStream >> NUMBITS_CRC;
        m_CurrentBlock = BLOCK__B;
        m_DecodeState = STATE_BLOCKSYNC; //next state is looking for blocks B,C, and D in sequence
      }
      break;

    case STATE_BLOCKSYNC: //Looking for 4 blocks in correct sequence to have good probability bit position is good
      m_CurrentBitPosition++;
      if (m_CurrentBitPosition >= NUMBITS_BLOCK)
      {
        m_CurrentBitPosition = 0;
        if (CheckBlock(BLK_OFFSET_TBL[m_CurrentBlock + m_BGroupOffset], false))
        { //bad chkword so go look for bit sync again
          m_DecodeState = STATE_BITSYNC;
        }
        else
        { //good chkword so save data and setup for next block
          m_BlockData[m_CurrentBlock] = m_InBitStream >> NUMBITS_CRC; //save msg data
          //see if is group A or Group B
          if ((BLOCK__B == m_CurrentBlock) && (m_BlockData[m_CurrentBlock] & GROUPB_BIT))
            m_BGroupOffset = 4;
          else
            m_BGroupOffset = 0;

          if (m_CurrentBlock >= BLOCK__D)
          { //good chkword on all 4 blocks in correct sequence so are sure of bit position
            m_CurrentBlock = BLOCK__A;
            m_BlockErrors = 0;
            m_DecodeState = STATE_GROUPDECODE;

            m_Decoder.DecodeRDS(m_BlockData);
          }
          else
            m_CurrentBlock++;
        }
      }
      break;

    case STATE_GROUPDECODE: //here after getting a good sequence of blocks
      m_CurrentBitPosition++;
      if (m_CurrentBitPosition >= NUMBITS_BLOCK)
      {
        m_CurrentBitPosition = 0;
        if (CheckBlock(BLK_OFFSET_TBL[m_CurrentBlock + m_BGroupOffset], USE_FEC))
        {
          m_BlockErrors++;
          if (m_BlockErrors > BLOCK_ERROR_LIMIT)
          {
            m_DecodeState = STATE_BITSYNC;
          }
          else
          {
            m_CurrentBlock++;
            if (m_CurrentBlock > BLOCK__D)
              m_CurrentBlock = BLOCK__A;
            if (BLOCK__A != m_CurrentBlock) //skip remaining blocks of this group if error
              m_DecodeState = STATE_GROUPRESYNC;
          }
        }
        else
        { //good block so save and get ready for next one
          m_BlockData[m_CurrentBlock] = m_InBitStream >> NUMBITS_CRC; //save msg data
          //see if is group A or Group B
          if ((BLOCK__B == m_CurrentBlock) && (m_BlockData[m_CurrentBlock] & GROUPB_BIT))
            m_BGroupOffset = 4;
          else
            m_BGroupOffset = 0;
          m_CurrentBlock++;
          if (m_CurrentBlock > BLOCK__D)
          {
            m_CurrentBlock = BLOCK__A;
            m_BlockErrors = 0;

            m_Decoder.DecodeRDS(m_BlockData);
          }
        }
      }
      break;

    case STATE_GROUPRESYNC: //ignor blocks until start of next group
      m_CurrentBitPosition++;
      if (m_CurrentBitPosition >= NUMBITS_BLOCK)
      {
        m_CurrentBitPosition = 0;
        m_CurrentBlock++;
        if (m_CurrentBlock > BLOCK__D)
        {
          m_CurrentBlock = BLOCK__A;
          m_DecodeState = STATE_GROUPDECODE;
        }
      }
      break;
  }
}

uint32_t cRDSRxSignalProcessor::CheckBlock(uint32_t SyndromeOffset, int UseFec)
{
  //! First calculate syndrome for current 26 m_InBitStream bits
  uint32_t testblock = (0x3FFFFFF & m_InBitStream); //!< isolate bottom 26 bits

  /*!
   * copy top 10 bits of block into 10 syndrome bits since first 10 rows
   * of the check matrix is just an identity matrix(diagonal one's)
   */
  uint32_t syndrome = testblock >> 16;
  for (int i = 0; i < NUMBITS_MSG; i++)
  {
    //! do the 16 remaining bits of the check matrix multiply
    if (testblock & 0x8000)
      syndrome ^= PARCKH[i];
    testblock <<= 1;
  }
  syndrome ^= SyndromeOffset; //!< add depending on desired block

  if (syndrome && UseFec) //!< if errors and can use FEC
  {
    uint32_t correctedbits = 0;
    uint32_t correctmask = (1 << (NUMBITS_BLOCK - 1)); //!< start pointing to msg msb

    //! Run Meggitt FEC algorithm to correct up to 5 consecutive burst errors
    for (int i = 0; i < NUMBITS_MSG; i++)
    {
      if (syndrome & 0x200) //!< chk msbit of syndrome for error state
      {
        //! is possible bit error at current position
        if (0 == (syndrome & 0x1F)) //!< bottom 5 bits == 0 tell it is correctable
        { //! Correct i-th bit
          m_InBitStream ^= correctmask;
          correctedbits++;
          syndrome <<= 1; //!< shift syndrome to next msb
        }
        else
        {
          syndrome <<= 1; //!< shift syndrome to next msb
          syndrome ^=
              CRC_POLY; //!< recalculate new syndrome if bottom 5 bits not zero and syndrome msb bit was a one
        }
      }
      else
      {
        //! no error at this bit position so just shift to next position
        syndrome <<= 1; //!< shift syndrome to next msb
      }
      correctmask >>= 1; //!< advance correctable bit position
    }
    syndrome &= 0x3FF; //!< isolate syndrome bits if non-zero then still an error
  }

  return syndrome;
}

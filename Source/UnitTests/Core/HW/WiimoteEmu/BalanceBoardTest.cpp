// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/HW/WiimoteCommon/WiimoteHid.h"
#include "Core/HW/WiimoteCommon/WiimoteReport.h"
#include "Core/HW/WiimoteEmu/DesiredWiimoteState.h"
#include "Core/HW/WiimoteEmu/Extension/BalanceBoard.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"

// Drives an emulated balance board the way a game does, over the HID interface, and checks the
// results against the publicly documented balance board protocol (WiiBrew "Wii Balance Board").
namespace
{
using Report = std::vector<u8>;

constexpr u8 BALANCE_BOARD_INDEX = 4;

// The extension controller's registers, as seen by a game: register space, address 0xa4xxxx.
constexpr u8 EXT_SLAVE_ADDRESS_BYTE = 0xa4;
constexpr u8 SPACE_REGISTER = 0x04;

constexpr u8 REPORT_WRITE_DATA = 0x16;
constexpr u8 REPORT_READ_DATA = 0x17;
constexpr u8 REPORT_MODE = 0x12;
constexpr u8 REPORT_READ_DATA_REPLY = 0x21;
constexpr u8 REPORT_BUTTONS_EXT19 = 0x34;

// Standard (reflected) CRC-32, as documented for the balance board calibration block.
u32 CRC32(const std::vector<u8>& data)
{
  u32 crc = 0xffffffff;
  for (const u8 byte : data)
  {
    crc ^= byte;
    for (int i = 0; i != 8; ++i)
      crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
  }
  return ~crc;
}

u16 BE16(const u8* p)
{
  return u16(p[0] << 8 | p[1]);
}

class BalanceBoardTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    m_board.SetInterruptCallback(
        [this](u8, const u8* data, u32 size) { m_reports.emplace_back(data, data + size); });
    m_board.EventLinked();
    Step(4);
  }

  void Send(const Report& report)
  {
    static_cast<WiimoteCommon::HIDWiimote&>(m_board).InterruptDataOutput(report.data(),
                                                                         u32(report.size()));
  }

  // One input update of the board, as happens once per Wii Remote polling period.
  void Step(int count = 1)
  {
    for (int i = 0; i != count; ++i)
    {
      WiimoteEmu::DesiredWiimoteState state;
      auto& hid = static_cast<WiimoteCommon::HIDWiimote&>(m_board);
      hid.PrepareInput(&state, WiimoteCommon::HIDWiimote::SensorBarState::Enabled);
      hid.Update(state);
    }
  }

  void WriteExtRegister(u8 address, u8 value)
  {
    Report report(22, 0);
    report[0] = REPORT_WRITE_DATA;
    report[1] = SPACE_REGISTER;
    report[2] = EXT_SLAVE_ADDRESS_BYTE;
    report[3] = 0x00;
    report[4] = address;
    report[5] = 1;
    report[6] = value;
    Send(report);
    Step();
  }

  std::vector<u8> ReadExtRegisters(u8 address, u16 size)
  {
    m_reports.clear();
    Send({REPORT_READ_DATA, SPACE_REGISTER, EXT_SLAVE_ADDRESS_BYTE, 0x00, address, u8(size >> 8),
          u8(size)});
    Step((size + 15) / 16 + 2);

    std::vector<u8> result;
    for (const Report& report : m_reports)
    {
      if (report.size() < 22 || report[0] != REPORT_READ_DATA_REPLY)
        continue;
      const u8 error = report[3] & 0xf;
      EXPECT_EQ(error, 0) << "read error at " << int(address);
      const int length = (report[3] >> 4) + 1;
      result.insert(result.end(), report.begin() + 6, report.begin() + 6 + length);
    }
    EXPECT_EQ(result.size(), size);
    return result;
  }

  std::optional<Report> GetDataReport()
  {
    m_reports.clear();
    Step(2);
    for (auto it = m_reports.rbegin(); it != m_reports.rend(); ++it)
    {
      if (it->at(0) == REPORT_BUTTONS_EXT19)
        return *it;
    }
    return std::nullopt;
  }

  void EnableExtensionReports()
  {
    // Unencrypted extension data, then buttons + 19 extension bytes, continuous.
    WriteExtRegister(0xf0, 0x55);
    WriteExtRegister(0xfb, 0x00);
    Send({REPORT_MODE, 0x04, REPORT_BUTTONS_EXT19});
    Step();
  }

  // Converts a raw sensor value to kg by interpolating the board's own calibration, the way a
  // game does (0..17 kg and 17..34 kg segments, the latter also used above 34 kg).
  double ToKilograms(int sensor, u16 raw) const
  {
    const double kg0 = m_cal[0][sensor], kg17 = m_cal[1][sensor], kg34 = m_cal[2][sensor];
    if (raw < kg17)
      return 17.0 * (raw - kg0) / (kg17 - kg0);
    return 17.0 + 17.0 * (raw - kg17) / (kg34 - kg17);
  }

  void ReadCalibration()
  {
    const auto cal = ReadExtRegisters(0x24, 0x18);
    for (int point = 0; point != 3; ++point)
    {
      for (int sensor = 0; sensor != 4; ++sensor)
        m_cal[point][sensor] = BE16(&cal[point * 8 + sensor * 2]);
    }
  }

  WiimoteEmu::BalanceBoard m_board{BALANCE_BOARD_INDEX};
  std::vector<Report> m_reports;
  std::array<std::array<u16, 4>, 3> m_cal{};
};
}  // namespace

TEST_F(BalanceBoardTest, ExtensionIdentifier)
{
  const auto id = ReadExtRegisters(0xfa, 6);
  const std::vector<u8> expected{0x00, 0x00, 0xa4, 0x20, 0x04, 0x02};
  EXPECT_EQ(id, expected);
}

TEST_F(BalanceBoardTest, CalibrationIsPlausibleAndChecksumMatches)
{
  // 0x20: info bytes, 0x24: 0 kg points, 0x2c: 17 kg points, 0x34: 34 kg points, 0x3c: CRC-32.
  const auto block = ReadExtRegisters(0x20, 0x20);
  // 0x60: reference temperature, 0x61: calibration version (1 = 0/17/34 kg reference loads).
  const auto block2 = ReadExtRegisters(0x60, 2);

  EXPECT_EQ(block[0], 0x01);
  EXPECT_EQ(block2[1], 0x01);

  ReadCalibration();
  for (int sensor = 0; sensor != 4; ++sensor)
  {
    EXPECT_LT(m_cal[0][sensor], m_cal[1][sensor]);
    EXPECT_LT(m_cal[1][sensor], m_cal[2][sensor]);
    // Roughly linear, like a real load cell.
    EXPECT_NEAR(m_cal[1][sensor] - m_cal[0][sensor], m_cal[2][sensor] - m_cal[1][sensor], 100);
  }

  // The CRC covers the 24 calibration bytes, then 0x20, 0x21, 0x60 and 0x61.
  std::vector<u8> crc_data(block.begin() + 4, block.begin() + 0x1c);
  crc_data.insert(crc_data.end(), {block[0], block[1], block2[0], block2[1]});
  const u32 stored_crc =
      u32(block[0x1c] << 24 | block[0x1d] << 16 | block[0x1e] << 8 | block[0x1f]);
  EXPECT_EQ(stored_crc, CRC32(crc_data));
}

TEST_F(BalanceBoardTest, DataReportCarriesDefaultWeight)
{
  ReadCalibration();
  EnableExtensionReports();

  const auto report = GetDataReport();
  ASSERT_TRUE(report.has_value());
  ASSERT_EQ(report->size(), 22u);

  // Bytes: report id, 2 button bytes, then TR, BR, TL, BL (big endian), temperature, -, battery.
  const u8* ext = report->data() + 3;
  double total = 0;
  for (int sensor = 0; sensor != 4; ++sensor)
  {
    const double kg = ToKilograms(sensor, BE16(ext + sensor * 2));
    // Centered balance: the weight is spread evenly.
    EXPECT_NEAR(kg, WiimoteEmu::BalanceBoardExt::DEFAULT_WEIGHT / 4, 0.1);
    total += kg;
  }
  EXPECT_NEAR(total, WiimoteEmu::BalanceBoardExt::DEFAULT_WEIGHT, 0.2);

  // A temperature of 0x7f or 0x80 means "not measured yet" to games.
  EXPECT_NE(ext[8], 0x7f);
  EXPECT_NE(ext[8], 0x80);
  // Matches the temperature the board was calibrated at, so there is no correction.
  EXPECT_EQ(ext[8], ReadExtRegisters(0x60, 1)[0]);

  // Full battery (games show 4 bars from 0x82).
  EXPECT_GE(ext[10], 0x82);
}

TEST_F(BalanceBoardTest, SensorConversionRoundTrips)
{
  using WiimoteEmu::BalanceBoardExt;
  for (const double kg : {0.0, 5.0, 17.0, 25.5, 34.0, 40.0})
  {
    EXPECT_NEAR(BalanceBoardExt::ConvertToKilograms(BalanceBoardExt::ConvertToSensorWeight(kg)), kg,
                0.01);
  }

  // Center of balance to the right and to the front moves weight to the top-right sensor.
  const auto sensors = BalanceBoardExt::CenterOfBalanceToSensors({0.5, 0.5}, 80.0);
  EXPECT_NEAR(sensors.data[0] + sensors.data[1] + sensors.data[2] + sensors.data[3], 80.0, 1e-9);
  EXPECT_GT(sensors.data[0], sensors.data[1]);
  EXPECT_GT(sensors.data[0], sensors.data[2]);
  EXPECT_GT(sensors.data[1], sensors.data[3]);
  const auto balance = BalanceBoardExt::SensorsToCenterOfBalance(sensors);
  EXPECT_NEAR(balance.x, 0.5, 1e-9);
  EXPECT_NEAR(balance.y, 0.5, 1e-9);
}

#include "pch.h"
#include "CppUnitTest.h"
#include "..\..\..\src\WaveTableManager.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace WavetableManagerTests
{
    TEST_CLASS(WavetableManagerTests)
    {
    public:
        TEST_METHOD(TestGenerateSample)
        {
            // Arrange
            WavetableManager manager(20.0, 20000.0, 1024, 10, 44100.0);
            manager.setWaveTypeAndFrequency(WaveType::SINE, 440.0);

            // Act
            SamplePrecision sample = manager.getNextSample();

            // Assert
            Assert::AreNotEqual(0.0f, sample); // Ad esempio, verifica che il sample non sia 0
        }

        TEST_METHOD(TestPhaseReset)
        {
            // Arrange
            WavetableManager manager(20.0, 20000.0, 1024, 10, 44100.0);
            manager.setWaveTypeAndFrequency(WaveType::SINE, 440.0);

            // Act
            for (int i = 0; i < 44100; ++i) {
                manager.getNextSample();
            }

            // Assert
            Assert::AreEqual(0.0f, manager.getNextSample()); // Puoi verificare valori noti o proprietà
        }
    };
}
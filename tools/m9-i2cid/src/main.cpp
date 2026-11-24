// ThinkNode M9: who answers at I2C address 0x7E on the peripheral bus (SDA 7, SCL 6)?
// Candidates: the QMI8658 motion sensor (0x6B) and the QMC6309 magnetometer (0x7C). Both chips also speak I3C,
// whose broadcast address is 0x7E. The test: point each chip at a register through its own address, then read
// at 0x7E without writing anything there, and see whose register comes back. Nothing is written to 0x7E in
// phase A; phase C writes one byte 0x00 there (the pointer to register 0, and in I3C terms a harmless code).
#include <Arduino.h>
#include <Wire.h>

static const int PIN_VEXT = 18; // LOW = on
static const int PIN_BL = 17;
static const int PIN_BUZZER = 9;
static const int PIN_GPS_EN = 11;
static const uint8_t QMI = 0x6B, QMC = 0x7C, X = 0x7E;
static TwoWire &bus = Wire1;
static uint32_t cycle = 0;

static bool probe(uint8_t addr)
{
    bus.beginTransmission(addr);
    return bus.endTransmission() == 0;
}

// sets the register pointer of a chip; stop: end with a stop condition
static int point(uint8_t addr, uint8_t reg, bool stop = true)
{
    bus.beginTransmission(addr);
    bus.write(reg);
    return bus.endTransmission(stop);
}

static void plainRead(uint8_t addr, uint8_t count, const char *what)
{
    uint8_t n = bus.requestFrom(addr, count);
    Serial.printf("M9ID     read %u at 0x%02x (%s): got %u:", count, addr, what, n);
    while (bus.available())
        Serial.printf(" %02x", bus.read());
    Serial.printf("\n");
}

static void dump(uint8_t addr, uint8_t from, uint8_t count, const char *what)
{
    Serial.printf("M9ID     registers 0x%02x.. of 0x%02x (%s):", from, addr, what);
    for (uint8_t i = 0; i < count; i++) {
        point(addr, from + i, false);
        if (bus.requestFrom(addr, (uint8_t)1) == 1)
            Serial.printf(" %02x", bus.read());
        else
            Serial.printf(" --");
    }
    Serial.printf("\n");
}

static void answers(const char *when)
{
    Serial.printf("M9ID   %s: answering:", when);
    for (uint8_t addr : {0x00, 0x51, 0x6A, 0x6B, 0x7C, 0x7D, 0x7E, 0x7F})
        if (probe(addr))
            Serial.printf(" 0x%02x", addr);
    Serial.printf("\n");
}

void setup()
{
    Serial.begin(115200);
    for (int cs : {39, 48, 16}) {
        pinMode(cs, OUTPUT);
        digitalWrite(cs, HIGH);
    }
    pinMode(PIN_BL, OUTPUT);
    digitalWrite(PIN_BL, HIGH);
    pinMode(PIN_VEXT, OUTPUT);
    digitalWrite(PIN_VEXT, LOW);
    delay(300);
    Wire.begin(20, 21, 100000);
    Wire.beginTransmission(0x6D);
    bool rev2 = Wire.endTransmission() == 0;
    Wire.end();
    pinMode(PIN_GPS_EN, OUTPUT);
    digitalWrite(PIN_GPS_EN, rev2 ? LOW : HIGH);
    tone(PIN_BUZZER, 2000, 120);
}

void loop()
{
    delay(4000);
    cycle++;
    bus.begin(7, 6, 100000);
    bus.setTimeOut(20);
    Serial.printf("M9ID cycle %lu\n", (unsigned long)cycle);
    answers("rail on");

    // A: whose register does a read at 0x7E return?
    dump(QMI, 0x00, 12, "QMI8658 by its own address");
    dump(QMC, 0x00, 12, "QMC6309 by its own address");
    struct {
        uint8_t qmi, qmc;
    } tries[] = {{0x00, 0x0B}, {0x01, 0x00}, {0x02, 0x0A}, {0x08, 0x00}, {0x00, 0x00}};
    for (auto &t : tries) {
        int a = point(QMC, t.qmc), b = point(QMI, t.qmi);
        Serial.printf("M9ID   A: QMC6309 pointed at 0x%02x (%d), then QMI8658 at 0x%02x (%d)\n", t.qmc, a, t.qmi, b);
        plainRead(X, 4, "the unknown");
        // the other order, in case the last chip addressed is the one that answers
        b = point(QMI, t.qmi);
        a = point(QMC, t.qmc);
        Serial.printf("M9ID   A: QMI8658 pointed at 0x%02x (%d), then QMC6309 at 0x%02x (%d)\n", t.qmi, b, t.qmc, a);
        plainRead(X, 4, "the unknown");
    }
    // does a read at 0x7E move a chip's pointer? read the chips plainly afterwards
    point(QMI, 0x00);
    point(QMC, 0x00);
    plainRead(X, 2, "the unknown");
    plainRead(QMI, 2, "QMI8658, pointer was 0x00");
    plainRead(QMC, 2, "QMC6309, pointer was 0x00");

    // B: the switched rail. Who goes away with it, and who comes back when?
    digitalWrite(PIN_VEXT, HIGH);
    delay(1000);
    answers("B: rail off for 1 s");
    digitalWrite(PIN_VEXT, LOW);
    delay(5);
    answers("B: rail on again, 5 ms");
    delay(100);
    answers("B: rail on again, 105 ms");
    delay(1000);
    answers("B: rail on again, 1.1 s");
    dump(QMC, 0x00, 2, "QMC6309 after the rail came back");

    // C: only in every third cycle, one byte 0x00 written to 0x7E, then a read of what follows
    if (cycle % 3 == 0) {
        int r = point(X, 0x00, false);
        Serial.printf("M9ID   C: pointer 0x00 written to 0x7e, result %d\n", r);
        plainRead(X, 12, "the unknown, from register 0x00");
        answers("C: after that");
        dump(QMI, 0x00, 4, "QMI8658 afterwards");
        dump(QMC, 0x00, 4, "QMC6309 afterwards");
    }
    bus.end();
}

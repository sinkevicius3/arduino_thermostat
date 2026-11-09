#include <EEPROM.h>
#include <LiquidCrystal.h>

#define LM_PIN A1  // lm35
#define LM_VCC_PIN A3
#define POT_PIN A5  // potentiometer
#define SAVE_BTN_PIN 2
#define SYSTEM_BTN_PIN 3
#define TRANS_PIN 13  // npn transistor
#define R_PIN 11      // rgb red
#define G_PIN 5       // rgb green
#define B_PIN 6       // rgb blue

const int default_target = 20;  // 20 degrees Celsius
const int target_range = 20;    // target +-
const int min_target = default_target - target_range;
const int max_target = default_target + target_range;
const float target_error = 3;

const unsigned long debounce_time = 250;

const int sample_tick_interval = 500;  // 500 * 10ms = 5000ms
const int display_tick_interval = 500; // 500 * 10ms = 5000ms

const byte EEPROM_MAGIC = 0xAC;
const byte EEPROM_VERSION = 1;

const int ADDR_MAGIC = 0;
const int ADDR_VERSION = 1;
const int ADDR_STATE = 2;
const int ADDR_TARGET = 3;  // integer uses two bytes

enum State { OFF, COLD, NORMAL, HOT };
State system_state = OFF;

int sample_ticks = 0;
int display_ticks = 0;

int target = default_target;
int temp_target = default_target;
float temperature = 0.0;

volatile bool save_btn_flag = false;
volatile bool system_btn_flag = false;
volatile bool timer_flag = false;

bool edit_mode = false;

volatile unsigned long last_save_interrupt_time = 0;
volatile unsigned long last_system_interrupt_time = 0;

// LiquidCrystal(rs, enable, d4, d5, d6, d7)
LiquidCrystal lcd(4, 7, 8, 9, 10, 12);

void setup() {
  Serial.begin(9600);

  // pinMode(LM_VCC_PIN, OUTPUT);
  // digitalWrite(LM_VCC_PIN, HIGH);

  DDRC  |= (1 << 3); // set PORTC third register (A3) to "OUTPUT"
  PORTC |= (1 << 3); // set PORTC third register (A3) to "HIGH"

  pinMode(SAVE_BTN_PIN, INPUT_PULLUP);
  pinMode(SYSTEM_BTN_PIN, INPUT_PULLUP);
  pinMode(TRANS_PIN, OUTPUT);
  pinMode(R_PIN, OUTPUT);
  pinMode(G_PIN, OUTPUT);
  pinMode(B_PIN, OUTPUT);

  lcd.begin(16, 2);

  noInterrupts();
  TCCR1A = 0;                           // reset
  TCCR1B = 0;                           // reset
  TCNT1 = 0;                            // timer itself
  OCR1A = 156;                          // 10 ms / 0.064 ms = 156.25
  TCCR1B |= (1 << WGM12);               // CTC mode: clear timer on compare match
  TIMSK1 |= (1 << OCIE1A);              // enable compare match A interrupt
  TCCR1B |= (1 << CS12) | (1 << CS10);  // set prescaler 1024, this also starts the timer
  interrupts();

  attachInterrupt(digitalPinToInterrupt(SAVE_BTN_PIN), save_btn_isr, FALLING);
  attachInterrupt(digitalPinToInterrupt(SYSTEM_BTN_PIN), system_btn_isr, FALLING);

  load_settings();

  if (system_state == OFF) {
    set_state(OFF);
    Serial.println("Restored: OFF");
  } else {
    Serial.println("Restored: ON");
    read_lm();
    system_state = OFF;  // force update_state() to apply read_lm() output
    update_state();
  }
}

void loop() {

  if (save_btn_flag) {
    save_btn_flag = false;

    if (system_state == OFF) {
      save_state(OFF);
      Serial.println("Saved: state OFF");
    } else if (!edit_mode) {
      edit_mode = true;
      temp_target = target;
      read_potentiometer();
      update_display();
      Serial.println("Editing target...");
    } else {
      edit_mode = false;
      target = temp_target;
      read_lm();
      sample_ticks = 0;
      update_state();
      update_display();

      save_target();
      save_state(system_state);

      Serial.print("Saved: target ");
      Serial.print(target);
      Serial.println(" C, state ON");
    }
  }

  if (system_btn_flag) {
    system_btn_flag = false;

    if (system_state == OFF) {
      read_lm();
      update_state();
      Serial.println("Thermostat ON");
    } else {
      set_state(OFF);
      Serial.println("Thermostat OFF");
    }
  }

  if (timer_flag) {
    timer_flag = false;

    ++sample_ticks;
    ++display_ticks;

    if (system_state != OFF && edit_mode) {
      int old_target = temp_target;
      read_potentiometer();  // only pot is read while editing
      if (temp_target != old_target) {
        update_display();  // show pot change immediately
      }
    }

    if (sample_ticks >= sample_tick_interval) {
      sample_ticks = 0;

      if (system_state != OFF && !edit_mode) {  // LM35 is not read while editing
        read_lm();
        update_state();
      }
    }

    if (display_ticks >= display_tick_interval) {
      display_ticks = 0;

      if (system_state != OFF) {
        update_display();
        print_status_to_serial();
      }
    }
  }
}

ISR(TIMER1_COMPA_vect) {
  timer_flag = true;
}

void save_btn_isr() {
  unsigned long now = millis();

  if (now - last_save_interrupt_time >= debounce_time) {
    save_btn_flag = true;
    last_save_interrupt_time = now;
  }
}

void system_btn_isr() {
  unsigned long now = millis();

  if (now - last_system_interrupt_time >= debounce_time) {
    system_btn_flag = true;
    last_system_interrupt_time = now;
  }
}

void discharge_adc() {
  ADMUX = (1 << REFS0) | 0x0F;  // sets reference to 5V, connects ADC input to GND
  ADCSRA |= (1 << ADSC);        // makes a read, sampling capacitor drains to GND
  while (ADCSRA & (1 << ADSC));  // wait until hardware clears ADSC bit
}

void read_lm() {
  for (int i = 0; i < 5; ++i) {
    discharge_adc();
  }

  analogRead(LM_PIN); // discard first read
  long sum = 0;
  for (int i = 0; i < 20; ++i) {
    sum += analogRead(LM_PIN);
  }

  temperature = (sum / 20.0) * (5.0 / 1023.0) * 100.0;
  for (int i = 0; i < 5; ++i) {
    discharge_adc();
  }
}

void read_potentiometer() {
  for (int i = 0; i < 5; ++i) {
    discharge_adc();
  }

  analogRead(POT_PIN);  // discard first read
  long sum = 0;
  for (int i = 0; i < 20; ++i) {
    sum += analogRead(POT_PIN);
  }
  
  temp_target = map(sum / 20.0, 0, 1023, min_target, max_target);
  for (int i = 0; i < 5; ++i) {
    discharge_adc();
  }
}

void update_state() {
  if (temperature <= target - target_error) {
    if (system_state != COLD) {
      set_state(COLD);
      Serial.println("Switched to COLD");
    }
  } else if (temperature < target + target_error) {
    if (system_state != NORMAL) {
      set_state(NORMAL);
      Serial.println("Switched to NORMAL");
    }
  } else {
    if (system_state != HOT) {
      set_state(HOT);
      Serial.println("Switched to HOT");
    }
  }
}

void set_state(State new_state) {
  system_state = new_state;

  switch (new_state) {
    case OFF:
      edit_mode = false;
      temp_target = target;
      digitalWrite(TRANS_PIN, LOW);
      setRGB(0, 0, 0);
      lcd.clear();
      lcd.noDisplay();
      break;

    case COLD:
      lcd.display();
      digitalWrite(TRANS_PIN, LOW);
      setRGB(0, 0, 255);
      update_display();
      break;

    case NORMAL:
      lcd.display();
      digitalWrite(TRANS_PIN, LOW);
      setRGB(0, 255, 0);
      update_display();
      break;

    case HOT:
      lcd.display();
      digitalWrite(TRANS_PIN, HIGH);
      setRGB(255, 0, 0);
      update_display();
      break;
  }
}

void setRGB(int r, int g, int b) {
  analogWrite(R_PIN, 255 - r);
  analogWrite(G_PIN, 255 - g);
  analogWrite(B_PIN, 255 - b);
}

void update_display() {
  if (system_state == OFF) {
    return;
  }

  lcd.setCursor(0, 0);
  lcd.print("Current: ");
  lcd.print((int)round(temperature));
  lcd.write((byte)223);
  lcd.print("C   ");

  lcd.setCursor(0, 1);
  lcd.print("Target: ");
  lcd.print(target);

  if (edit_mode) {
    lcd.print(" [");
    lcd.print(temp_target);
    lcd.print("]");
  }

  lcd.print("      ");
}

void print_status_to_serial() {
  Serial.print("Curr: ");
  Serial.print((int)round(temperature));
  Serial.print(" C, Target: ");
  Serial.print(target);
  Serial.print(" C, System: ");

  switch (system_state) {
    case OFF:
      Serial.println("OFF");
      break;
    case COLD:
      Serial.println("COLD");
      break;
    case NORMAL:
      Serial.println("NORMAL");
      break;
    case HOT:
      Serial.println("HOT");
      break;
  }
}

void load_settings() {
  int saved_target;
  EEPROM.get(ADDR_TARGET, saved_target);
  byte saved_state = EEPROM.read(ADDR_STATE);

  bool valid = (EEPROM.read(ADDR_MAGIC) == EEPROM_MAGIC) && (EEPROM.read(ADDR_VERSION) == EEPROM_VERSION) && (saved_target >= min_target) && (saved_target <= max_target) && (saved_state <= HOT);

  if (!valid) {
    target = default_target;
    system_state = OFF;

    EEPROM.update(ADDR_MAGIC, EEPROM_MAGIC);
    EEPROM.update(ADDR_VERSION, EEPROM_VERSION);
    EEPROM.put(ADDR_TARGET, target);
    EEPROM.update(ADDR_STATE, (byte)OFF);

    Serial.println("EEPROM empty or corrupted, defaults saved");
  } else {
    target = saved_target;
    system_state = (State)saved_state;

    Serial.print("Loaded target temperature: ");
    Serial.println(target);
    Serial.print("Loaded previous state: ");
    Serial.println(saved_state);
  }

  temp_target = target;
}

void save_target() {
  EEPROM.put(ADDR_TARGET, target);
}

void save_state(State new_state) {
  EEPROM.update(ADDR_STATE, (byte)new_state);
}
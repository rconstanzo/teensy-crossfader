//////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////
 /*
 *  USB-MIDI high resolution (14-bit) DJ-style crossfader.
 *  by Rodrigo Constanzo, http://www.rodrigoconstanzo.com // rodrigo.constanzo@gmail.com
 *  
 *  coded for
 *     TT Electronics PS45G-C1LBR10KN fader: https://www.digikey.com/product-detail/en/tt-electronics-bi/PS45G-C1LBR10KN/987-1402-ND/2620671
 *     TeensyLC: https://www.pjrc.com/teensy/teensyLC.html
 *     Adafruit ADS1115: https://www.adafruit.com/product/1085
 *  
 *  EXPLANATION
 *  -----------
 *  The code takes analog readings from the ADS1115 external ADC and scales, smooths, and
 *  constrains the output before sending it as high resolution MIDI CCs (using two CCs as
 *  MSB and LSB). The code includes a calibration routine that takes the minimum and maximum
 *  readings of the fader and stores them in the internal EEPROM. To use the calibration
 *  routine, you must send Program Change message 13 followed by 69, both on channel 11. The
 *  initial state of the device can be reset by sending Program Change message 10 followed by 
 *  110.
 *  (example Max/MSP code below)
 *  
 *  last update on 6/5/2022
 */
//////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////

// required libraries
#include <Wire.h>
#include <Adafruit_ADS1015.h>
#include <ResponsiveAnalogRead.h>
#include <EEPROM.h>


// declared variables

// define MIDI channel
const int midi_channel = 11;

// define CC for the first CC (MSB)
const int cc_msb = 1;

// define CC for the second CC (LSB)
// **this should be 32 higher than the MSB if you want it to work properly in apps that accept 14-bit MIDI**
const int cc_lsb = 33;

// internal LED for calibration/reset status
const int led = 13;

// declare the available sensor range
// using a TeensyLC and ADS1115 gives you 80% of 4.096v (3.3v is 80%) so that makes 26400 80% of 32768
const int sensor_range = 26400;

// set output range (14-bit = 16383)
const int output_range = 16383;

// ints for filtering repeats
int current_value = 0;
int previous_value = 0;

// flag to check if in calibration mode
int calibrate_flag = 0;

// flag to check if in reset mode
int reset_flag = 0;

// store whether device has ever been calibrated in order to allow default settings
int has_been_calibrated = EEPROM.read(10);

// by default leave 5% slop at the top end and 2.5% slop at the bottom
int calibrate_min = 0 + (sensor_range / 40);
int calibrate_max = sensor_range - (sensor_range / 20);

// instantiate adafruit library
Adafruit_ADS1115 ads;

// variable to store sensor reading
int16_t adc0;

// initialize sensor reading
ResponsiveAnalogRead analog(0, true);


/////////////////////////// SETUP ///////////////////////////

void setup(void) 
{
  // Serial.begin(9600);

  // look for program change messages (for calibration routine)
  usbMIDI.setHandleProgramChange(OnProgramChange);

  // if the device has been calibrated, use the stored settings instead of default settings
  if (has_been_calibrated == 1) {
    calibrate_min = constrain((BitShiftCombine(EEPROM.read(0), EEPROM.read(1))), 0, sensor_range);
    calibrate_max = constrain((BitShiftCombine(EEPROM.read(2), EEPROM.read(3))), 0, sensor_range);
  }

  /*
  Serial.print("The initial calibrate_min value is");
  Serial.print(" ");
  Serial.println(calibrate_min);

  Serial.print("The initial calibrate_max value is");
  Serial.print(" ");
  Serial.println(calibrate_max);
  */

  // initialize ADS1115 library
  ads.begin();

  // set ADS1115 gain to 1x, to get the maximum possible range with the 3.3v Teensy
  ads.setGain(GAIN_ONE);        // 1x gain   +/- 4.096V  1 bit = 2mV      0.125mV

  // set smoothing resolution
  analog.setAnalogResolution(sensor_range);

  // flash LED on startup for troubleshooting purposes
digitalWrite(led, HIGH);
delay(500);
digitalWrite(led, LOW);

  // reset sketch to initial state
  // EEPROM.write(10,255);

}


/////////////////////////// LOOP ///////////////////////////

void loop(void) {
  
  // read analog pin from ADS1115
  adc0 = ads.readADC_SingleEnded(0);
  analog.update(adc0);

  // scale the available reading range to the available output range (typically 14-bit MIDI)
  current_value = map(analog.getValue(), calibrate_min, calibrate_max, 0, output_range); 

  // constrain value after scaling to avoid exceeding the available range
  current_value = (constrain(current_value, 0, output_range));

  // filter repeats
  if (current_value != previous_value) {
      if (current_value >> 7 != previous_value >> 7) {
        usbMIDI.sendControlChange(cc_msb, current_value >> 7, midi_channel);
      }
      usbMIDI.sendControlChange(cc_lsb, current_value & 127, midi_channel); 
      previous_value = current_value;
  }

  // update MIDI
  usbMIDI.read();

  // flash the onboard LED for 100ms every 5 seconds, for troubleshooting purposes
  /*
  static long prevT = 0;
  static const long blinkT=5000;

  unsigned long currT = millis();
  digitalWrite(led, (currT-prevT)>=blinkT ? HIGH : LOW );
  if(currT-prevT>=(blinkT+100)) prevT=currT; 
  */
  
}


/////////////////////////// FUNCTIONS ///////////////////////////

// create a single 16-bit int from two 8-bit ints
int BitShiftCombine(unsigned char x_high, unsigned char x_low) {
  int combined; 
  combined = x_high;              //send x_high to rightmost 8 bits
  combined = combined<<8;         //shift x_high over to leftmost 8 bits
  combined |= x_low;                 //logical OR keeps x_high intact in combined and fills in                                                             //rightmost 8 bits
  return combined;
}

// calibration and reset routines to run when receiving program change messages on channel 11
void OnProgramChange(byte channel, byte program) {
  // offset midi program channel message since they count from 1 instead of 0
  program++;
  if (channel == 11) {
    if (program == 13) {
      calibrate_flag = 1;
      } else if (calibrate_flag == 1) {
         if (program == 69) {

          // enable LED notification of status
          digitalWrite(led, HIGH);
          // Serial.println("calibrating!");
          
          while (calibrate_flag == 1) {
            
            // actual calibration function 
            calibrateSensor();

            // terminate while loop
            calibrate_flag = 0;      
          }

          // turn off notification LED
          digitalWrite(led, LOW);

          // add a little buffer at each extreme to ensure the values can achieve the full range
          calibrate_min += 5;
          calibrate_max -= 20;
            
          // clamp and slightly limit reading values and write new min/max values to EEPROM
          EEPROM.write(0, (constrain(calibrate_min, 0, sensor_range / 5)) >> 8);
          EEPROM.write(1, (constrain(calibrate_min, 0, sensor_range / 5)) & 255);
          EEPROM.write(2, (constrain(calibrate_max, sensor_range - sensor_range / 5, sensor_range) - 10) >> 8);
          EEPROM.write(3, (constrain(calibrate_max, sensor_range - sensor_range / 5, sensor_range) - 10) & 255);
          
          // write the fact that device has been calibrated to EEPROM
          EEPROM.write(10, 1);

          /*
          Serial.println("done calibrating!");
          Serial.println(calibrate_min);
          Serial.println(calibrate_max);
          */

         } else {
          calibrate_flag = 0;
         }
    }
  }
    // reset initial state
    if (channel == 11) {
      if (program == 10) {
        reset_flag = 1;
        } else if (reset_flag == 1) {
          if (program == 110) {

          digitalWrite(led, HIGH);
          delay(100);
         
          // write the fact that device has been reset to EEPROM
          EEPROM.write(10, 255);

          // reset minimum and maximum to initial values
          calibrate_min = 0 + (sensor_range / 40);
          calibrate_max = sensor_range - (sensor_range / 20);

          // Serial.println("values reset!");
          
          // turn off notification LED
          digitalWrite(led, LOW);

          } else {
            reset_flag = 0;
          }
    }
  }
}

void calibrateSensor() {
  // declare variables to use in calibration routine
  bool calib_bool = true;
  bool schmitt = false;
  int schmitt_count = 0;

  // set minimum and maximum to absurd values
  calibrate_min = 1000000;
  calibrate_max = -100;
  
  while(calib_bool) {
    adc0 = ads.readADC_SingleEnded(0);
    analog.update(adc0);
    int reading = analog.getValue();
    
    // Serial.println("current reading value:");
    // Serial.println(reading);

    // update the minimum and maximum values
    if (reading < calibrate_min) {
      calibrate_min = reading;
    }
    if(reading > calibrate_max) {
      calibrate_max = reading;
    }

    // use a schmitt trigger to count how many times the full range of the fader has been moved
    if (reading > sensor_range - (sensor_range / 4) && schmitt == false) {
      schmitt = true;
    }
    if (reading < sensor_range / 4 && schmitt == true) {
      schmitt = false;
      schmitt_count++;
    }

    // stop calibration routine after 4 passes have been made
    if(schmitt_count >= 4) {
      calib_bool = false;
    }
  }
}


//////////// EXAMPLE MAX CODE //////////////////////
/*

<pre><code>
----------begin_max5_patcher----------
2397.3ocyakzaiibE9r8uhJB4fsgZ20JKxfI.IYNLWlfbnCPPvzCLJQURlSS
QJvEuLSl92dpERJRsPURhhSa.KSRwh06689dK0qn+sauYxrz2j4S.+EvOAt4
le61atwbI8Eto57alrR7VXrH2baSBSWsRlTLYp86JjuUXt9yQKeFjIySiKKh
RS.KhSEM2UR4pzxhXYg4Y.qtZzbyHSm8KevGUeqqEEgOGkr7oLYXgUvvbzir
o.hG9Q3T.Bwz+ACeDB94MO+nj5GORese+1a0eL8BQURZTtDjl7wzEK.gkEEJ
Iq9lhiRjgokIl6D6JR4A8fTODRiMu.OMf4T8IDxX.z7PQrDTjBfOp+D8nylN
N2ASGhqgR.d7rbH5GlEU.dQDWJcGKriiELmnQg8ywAKZfj+bzhBvp7YeLNeF
HJQYjDfbkDpLaUXcaWPk7LSlcgzUROZDR.znQvl+f77GO9ZlbYYrHCbGzPWw
76AqhlG0B9WHtg8faqiI1Kv.a3UvMcdjHd+rVztxpm+FYMSrRVHydRlHlEKa
CN6yn380RKHlXiP2HyGFmVyKEpOgNJD9Uo4JFezxjnEQghjBfhdeY1Su9hRQ
wlvtHhAn7QjFGKECNR6KFFB60Bo93wCoyyDuphX8hx6bkDnS2zJU5wAEturl
XCN7XFCGBGLdQlWHRBeGrVY3xAKEQIaCryyDx5qFABxfOOJwXBgCuIbUYbQT
dbz7MIO5Jx38XffmULnISU+1H44xBknqjE62ZfFpExBSSJlKJDsiCdXMDMvV
LkkwSGG+Yk8VkU5jKhh42WgGHxNV7gkdqREndFNarYdmswdlP4bzH3KRyVIL
nz6nvm3YLmLK7wig0b6BpLUXzYgMmo+MsGqsuMLFiar1VN7v5eepVa7XZs8s
KAhQvWKqc6ZqLFSknutrncPkiVxEkckJ4pF97lpsF5ZtRjupPvNb8GzEQ+ny
0bRanvmOHqBPSw8aiwCY95xjPsqbdCAPkRYgHTdXq+97egsGddzuZFtgwdrn
X9XaQJLZMza4djTjn3Rlo4um0cE.WoxWxRWAVHZkn+rzGb7YqOrLbjWvXpN5
tv3iF.jhFn.fGVMn6svUJb2oBV30CrjpZwHdei.VRvU2xdMA69ijKeacF3N9
iT0p7vOrN806vOxQp.O5eoe9ySu6CHF+wGhSWdmh0wQLTv82e+duy69yKPOf
Twrte6a+9Gze2e5uBueu0BQcMQhZEoGLSxjNACpiqf5MtRcw+POaYxllE68G
bbVUMjR2aFNouxCwAXSSJrq4kvGu05pWY6rLQg6sSkzWyCwPirWADl2vCjUx
7bwR4tsF1rKBPmq0g.cfgtOD5a.EmxcIB.dHQHQuIBNCPbvYBPaHtJ.RoCO.
OPHtu66.b2QG6fnKJovoDV9MqBaj.35kZPoXof+lNTE3yS92RYR96fuOKMO2
Tw1mm3reHlb7tKvq1bFLd7.oH7KfE.h6VR74wSY1xK8XVGQzngPcJwu5b0Hn
CST0cHVmmZ2kT6ZdwJc.GZya3siNvwzhmi1Q9qyEge0Y5Jpud0yP17eV5JkU
+4U2V9vWclmhnGyPdNFvJfaC1xY1cdabLfGxANJ4KtqU3WQkREq1+bY0mUaz
p5kv9U.z84eyuftFMspa4tr1iJm8pFrfQvcaA97zUhnDC1aTvqRmu8B+aohp
tnX970opDm4O8ZTwyOEVl8hrU+5seLs4D0Od5C72bUTyG1q.94giR9Q.xi3S
buQVHjqMxxcVYcGqQ1k9QGyXsmyl2f9FZyap6KzUbyaN.04+4LmIvsBJOYBC
tZ2Y43wjwb.0QXgp5E.BrwA430mMz5kp5usbAl2XFcuWsBgbJpEzUhtT4hfG
B0hYrl5U25sAzL85q2UWkmVlEVCi58DDzJ8hLuHJQXxOt4lLY.acWqhpxkTg
KrIaNIfp+Ci62bVyHRyzA1zQk1qM0UoUWg1wkVsvh9iapg6N0nKapwtXjzak
O3PIkGV5vVxygoC5ro3F5vfqXbSZg62lfuLaBxgodK4afPM0Elnd2euX1fSf
bKw4.jAZfcazfMmMzNn0HtegkM.pEpKpkZc2EMSAt.ogXhPLWfD0FX8hlIL1
oDNCBl3NLU3gvQg6p16RmHBzEHQFfYBGLVyD0ELoeYYObTFhsuJHt88ijzbx
fmJ1AQkDLDpeW77gik1Gy1ehrKKhM1kfNACPLGRfiSTWkos3Zw50uHyxqtYy
bnVxwujZT.9SMmFkXO0T7+jL4KQ02u8FDYp0BTnVHPYls5927r8Zzz6lrjxn
JpoBcpoLu383sKneyBCJmGk9oBQQY9S+SYRocMBJPsPTFWzUSLa4hn33vzXq
3cSy2nVZT0xYlX+1oUWt4d0sB.GPQHcGfdjfHbrogPD0ALVqBrqFCpdPTFM.
h02J0CyoLyQ9XBiT00nNCCuYtfn.6L.C7gT6QpKg5zOBktT++Ngo.NyKaU0U
WmktNMq1jpFZPy8WVjtLSLORZ2haCm0Zhspaa+STeqQsazIU5TSSYpGSahWi
w3GKCE8Y.DggpGbGkJAqTPFzQYJjZNhauV69tHikq1dnbeOOtQg5CQ991iTO
ELt6XyUC1ziytiFC8ryGxiisVCeHDQ6NX8ZlMi6onDMwuoKgXR.FYrP3FSLl
6SIcsPNQ5ZLHGf24HWnKyyugu4ygLee6Q6Hfay77pMCd5e31irOoqBwqQM0A
yAPJha3E9ALJmass3.q8l40pgbsFEkyPDycfCHpAZsNHkgtqMocqETw69Wqk
IfOIRxAeRtJZVZ77I0Q9tPGhuX6g4+QGtqOGiVn.0s4xnMcV9BkkUh27zcK5
z7OszlFtNuhA5jKRqFnusOwgAroLkACvl9D4nlWweTwkwAsiAo8ooLzPaG57
JOreA6bzpmajlVwLp8428ndhYv80Yzlt2i1cXeSliqq2gaVQamPclcYrYspG
PgYRvPxrrc68eDWJq6Vq6ogQbOpMXei.FnuD5JHfYoulbxR3F4h03aRtRR32
+t3zEPHVkoxFjjSHXquCFgzuFSCs.9CYR4YHgFsGQ+istGcIBCuv8ekwwoud
xRmuJSM2F4.4iLw2XJMJa2T206uvOY1gAV.D5OT.v7l29CpJ.NYwWSEsaRLS
krjYPBpczrAQ.yrETnkv9WoyoIa6qXJOpmGInhEyqJASYOBNe9rcIjlsmRin
70Uui9lcw51e+1++Xf7eC
-----------end_max5_patcher-----------
</code></pre>


*/

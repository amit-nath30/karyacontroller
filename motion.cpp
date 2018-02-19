
#include "motion.h"
#include "common.h"
#include "timer.h"
#include "config_pins.h"
#include "temp.h"
#include <stdint.h>


#ifndef ISPC
#include<arduino.h>
#else
#include <graphics.h>
#include <stdlib.h>
#include <stdio.h>
#include <conio.h>
#endif

//#define steptiming

#define MINIMUMSTEPS 20 // a path less than this steps will merged with next move, to much merged will make it ugly, normally i think this must be less that nozzle diameter steps
#define MINCORNERSPEED 3 // minimum cornering speed
#define FASTINVSQRT  // less precice speed faster 4us
#define FASTDISTANCE //
#define DEVIATE 1//;0.01*100 - for centripetal corner safe speed
#define INTERPOLATEDELAY  // slower 4-8us
#define ADVANCEEXTRUDER
#ifdef USETIMER1
#define CLOCKCONSTANT 1000000.f
#else
#define CLOCKCONSTANT 1000000.f
#endif
#define XYJERK 25
#define ZJERK 5

#define JERK1 //centripetal corner , still not good, back to repetier style jerk

#define X 0
#define Y 1
#define Z 2
#define E 3

// idea: a simple ring buffer contain a simple command to 4 motor stepper
// command: -set direction param : 4 bit send to direction pin
//          -motor step : 4 bit, if active then send step command
// so 2 bit command, and 4 bit for parameter
// delay: at least for lowest 1mm/s and 100step/mm is 10000, so i think int 16 bit is enough
// so 3 bit, and at least to prevent runout buffer because of heavy calculation, need at least
// for 200mm/s at 100step/mm need -> 1000000/(200x100) 50us
// and from what i see, heavy calculation need atleast 1500us, so need 30buffer x 3 =90bytes ram
//
// the new motion loop will read from this buffer, as simple as possible and must able to be called from anywhere code
// this mean it will easy to port to timer interrupt model
// ======
// currently, i just implemented the spreading calculation in some bresenham steps to prevent long delay when a path finished
// actually, i think the previous idea is simpler, i will implement it later.
//

#define NUMCMDBUF 30


uint8_t homingspeed;
uint8_t homeoffset[4] ;
uint8_t xback[4];
uint8_t head, tail, tailok;
int maxf[4];
float f_multiplier, e_multiplier;
int accel;
int i;
int mvaccel;
uint16_t ax_max[3];
float stepdiv, stepdiv2;
int32_t totalstep;
uint32_t bsdx[NUMAXIS];
int8_t  sx[NUMAXIS];
int32_t dlp, dl;
int8_t checkendstop;
int8_t endstopstatus[3];


float stepmmx[4];
float cx1, cy1, cz1, ce01;
tmove move[NUMBUFFER];


#ifdef DRIVE_DELTA
// dummy step/mm for planner, 100 is good i think
#define FIXED2 100.f
#define IFIXED2 1.f/(FIXED2)
#define Cstepmmx(i) FIXED2
#else
#define Cstepmmx(i) stepmmx[i]
#endif

// to calculate delay from v^2, fast invsqrt hack
#define INVSCALE 6
#define INVSCALE2 (1<<(INVSCALE/2))

float InvSqrt(float x) {
#ifdef FASTINVSQRT
  int32_t* i = (int32_t*)&x;           // store floating-point bits in integer
  *i = 0x5f335a86  - (*i >> 1);    // initial guess for Newton's method
#else // if FAST then bypass newtons method
  float xhalf = 0.5f * x;
  int32_t i = *(int32_t*)&x;            // store floating-point bits in integer
  i = 0x5f375a86  - (i >> 1);    // initial guess for Newton's method
  x = *(float*)&i;              // convert new bits into float
  x = x * (1.5f - xhalf * x * x); // One round of Newton's method , disable if want faster but not precise
#endif
  return x;
}

#define INVSCALE3 1 << INVSCALE
#define xInvSqrt(n) n>(INVSCALE3)?stepdiv2*InvSqrt(n):stepdiv
#define xInvSqrt2(n,stepd,stepd2) n>(INVSCALE3)?stepd2*InvSqrt(n):stepd
//#define xInvSqrt(n) n>(INVSCALE3)?stepdiv2/sqrt(n):stepdiv
/*
    =================================================================================================================================================
    DELTA PRINTER FUNCTIONS
    =================================================================================================================================================
*/
uint32_t approx_distance_3(uint32_t dx, uint32_t dy, uint32_t dz) {
  uint32_t min, med, max, approx;

  if ( dx < dy ) {
    min = dy;
    med = dx;
  } else {
    min = dx;
    med = dy;
  }

  if ( dz < min ) {
    max = med;
    med = min;
    min = dz;
  } else if ( dz < med ) {
    max = med;
    med = dz;
  } else {
    max = dz;
  }

  approx = ( max * 860 ) + ( med * 851 ) + ( min * 520 );
  if ( max < ( med << 1 )) approx -= ( max * 294 );
  if ( max < ( min << 2 )) approx -= ( max * 113 );
  if ( med < ( min << 2 )) approx -= ( med *  40 );

  // add 512 for proper rounding
  return (( approx + 512 ) >> 10 );
}
uint32_t approx_distance(uint32_t dx, uint32_t dy) {
  uint32_t min, max, approx;

  // If either axis is zero, return the other one.
  if (dx == 0 || dy == 0) return dx + dy;

  if ( dx < dy ) {
    min = dx;
    max = dy;
  } else {
    min = dy;
    max = dx;
  }

  approx = ( max * 1007 ) + ( min * 441 );
  if ( max < ( min << 4 ))
    approx -= ( max * 40 );

  // add 512 for proper rounding
  return (( approx + 512 ) >> 10 );
}
uint32_t sqrt32(uint32_t x) {
  //zprintf(PSTR("SQRT: %d\n"), fi(x));

  uint32_t c = 0x8000;
  uint32_t g = 0x8000;

  for (;;) {
    if (g * g > x)
    {
      g ^= c;
    }
    c >>= 1;
    if (c == 0)
    {
      return g;
    }
    g |= c;
  }

}
#ifdef DRIVE_DELTA
float sgx[NUMAXIS]; // increment delta each segment

float delta_diagonal_rod;
float DELTA_DIAGONAL_ROD_2;

float delta_tower1_x;
float delta_tower1_y;
float delta_tower2_x ;
float delta_tower2_y ;
float delta_tower3_x ;
float delta_tower3_y ;

float delta_radius         = (DELTA_RADIUS );



#endif // delta

/*
    =================================================================================================================================================
    RESET_MOTION
    =================================================================================================================================================
  Reset all variable
*/
// keep last direction to enable backlash if needed
int8_t lsx[4] = {0, 0, 0, 0};

void reset_motion() {
  e_multiplier = f_multiplier = 1;

  homingspeed = HOMINGSPEED;
  homeoffset[0] = XOFFSET;
  homeoffset[1] = YOFFSET;
  homeoffset[2] = ZOFFSET;
  homeoffset[3] = EOFFSET;

  maxf[0] = XMAXFEEDRATE;
  maxf[1] = YMAXFEEDRATE;
  maxf[2] = ZMAXFEEDRATE;
  maxf[3] = E0MAXFEEDRATE;



  accel = XACCELL;

  mvaccel = XMOVEACCELL;

  stepmmx[0] = XSTEPPERMM;
  stepmmx[1] = YSTEPPERMM;
  stepmmx[2] = ZSTEPPERMM;
  stepmmx[3] = E0STEPPERMM;

  ax_max[0] = XMAX * 10;
  ax_max[1] = YMAX * 10;
  ax_max[2] = ZMAX * 10;

  checkendstop = 0;
  endstopstatus[0] = 0;
  endstopstatus[1] = 0;
  endstopstatus[2] = 0;
  head = tail = tailok = 0;
  cx1 = 0;
  cy1 = 0;
  cz1 = 0;
  ce01 = 0;
#ifdef ISPC
  tick = 0;
#endif
  lsx[0] = 0;
  lsx[1] = 0;
  lsx[2] = 0;
  lsx[3] = 0;
  xback[0] = MOTOR_X_BACKLASH;
  xback[1] = MOTOR_Y_BACKLASH;
  xback[2] = MOTOR_Z_BACKLASH;
  xback[3] = MOTOR_E_BACKLASH;

  e_multiplier = 1;
}

void preparecalc() {
#ifdef DRIVE_DELTA
  delta_diagonal_rod   = (DELTA_DIAGONAL_ROD);
  DELTA_DIAGONAL_ROD_2 = (DELTA_DIAGONAL_ROD) * (DELTA_DIAGONAL_ROD);

  delta_tower1_x       = (COS(DegToRad(TOWER_X_ANGLE_DEG)) * DELTA_RADIUS);
  delta_tower1_y       = (SIN(DegToRad(TOWER_X_ANGLE_DEG)) * DELTA_RADIUS);
  delta_tower2_x       = (COS(DegToRad(TOWER_Y_ANGLE_DEG)) * DELTA_RADIUS);
  delta_tower2_y       = (SIN(DegToRad(TOWER_Y_ANGLE_DEG)) * DELTA_RADIUS);
  delta_tower3_x       = (COS(DegToRad(TOWER_Z_ANGLE_DEG)) * DELTA_RADIUS);
  delta_tower3_y       = (SIN(DegToRad(TOWER_Z_ANGLE_DEG)) * DELTA_RADIUS);

  delta_radius         = (DELTA_RADIUS );

#endif

}

int32_t mcx[NUMAXIS];
/*
    =================================================================================================================================================
    MOTOR CLASS
    =================================================================================================================================================
*/

#ifdef USETIMER1
#define CORELOOP
#else
#define CORELOOP if(m)motionloop();
#endif
#define FASTAXIS(n) n->status >> 4
//if(m)coreloop();
#include "motors.h"

int mb_ctr;
int32_t bufflen() {
  mb_ctr = head >= tail ? head - tail : (NUMBUFFER + head) - tail; // 5+0 - 4
  return mb_ctr;
}

void power_off() {
  motor_0_OFF();
  motor_1_OFF();
  motor_2_OFF();
  motor_3_OFF();

}
/*
    =================================================================================================================================================
    PREPARERAMP
    =================================================================================================================================================
  Bertujuan mengkalkulasi berapa tangga aselerasi awal (rampup) dan akhir (rampdown)
  rampup belum tentu speed naik, bisa jadi speed turun.

  Profil dari gerakan ditentukan oleh fs (fstart), fn (f nominal), fe (f end)
  dengan aselerasi yang ditentukan, maka dihitung berapa rampup, dan down

  Apabila rampup dan down bertemu, atau melebihi jarak totalstep, maka perlu dikalkulasi lagi fs,fn dan fe supaya ramp bisa terpenuhi
  Untuk rampdown, harusnya rekursif ke belakang apabila tidak cukup jarak rampdown. namun implementasi sekarang
  hanya melakukan kalkulasi ulang aselerasi supaya rampdown maksimal ya sama dengan totalstep
*/
int32_t currdis, prevdis;
#define ramplen(oo,v0,v1,a,stepmm) oo=((int32_t)v1-(int32_t)v0)*stepmm/(a);
#define ramplenq(oo,v0,v1,stepa) oo=((int32_t)v1-(int32_t)v0)*stepa;
#define speedat(v0,a,s,stp) ((int32_t)a * s / stp + (int32_t)v0)
#define accelat(v0,v1,s) ((int32_t)v1  - (int32_t)v0 ) / (2 * s)
#define sqr(x) x*x
void prepareramp(int32_t bpos)
{
  tmove *m;
  m = &move[bpos];
  if (m->status & 4)return; // already calculated

  float t;
  int faxis = FASTAXIS(m);
  int32_t ytotalstep = labs(m->dx[faxis]);
#define stepmm  Cstepmmx(faxis)

  float stepa = stepmm / (m->ac);
  CORELOOP

  ramplenq(m->rampup, m->fs, m->fn, stepa);
  ramplenq(m->rampdown, m->fe, m->fn, stepa);

  CORELOOP

  //#define preprampdebug

#ifdef preprampdebug
  zprintf(PSTR("PREPARE RAMP FX %d Stepmm %f Bpos:%d\n"), fi(faxis), ff(stepmm), fi(bpos));
  zprintf(PSTR("Bpos:%d\n"), (int32_t)bpos);
  zprintf(PSTR("----------1-----------\nRU:%d Rd:%d TS:%d\n "), m->rampup, m->rampdown, ytotalstep);
  zprintf(PSTR("FS:%f FN:%f FE:%f\n"), ff(m->fs), ff(m->fn), ff(m->fe));
#endif
  // New generic algorithm, to calculate rampup and rampdown simpler, and faster
  // if rampup and ramp down crossing
  if (m->rampup + m->rampdown > ytotalstep) {
    if (m->rampup) {
      // if crossing and have rampup
      int32_t r = ((m->rampdown + m->rampup) - ytotalstep) >> 1;
      m->rampup -= r;
      //if (m->rampup<0)m->rampup=0;
      m->rampdown -= r;
    }
    // check if still larger that totalstep
    if (m->rampup > ytotalstep) {
      // adjust speed if acceleration up cannot reach requested speed
      m->fn = speedat(m->fs, m->ac, ytotalstep, stepmm);
      CORELOOP
      m->fe = m->fn;
      m->rampdown = 0;
      m->rampup = ytotalstep;
      CORELOOP
#ifdef preprampdebug
      zprintf(PSTR("========2========\nRU:%d Rd:%d\n"), m->rampup, m->rampdown);
      zprintf(PSTR("FS:%f FN:%f FE:%f\n"), ff(m->fs), ff(m->fn), ff(m->fe));
#endif
    } else if (m->rampdown > ytotalstep) {
      // ---------adjust deceleration
      // BACKPLANNER iterate back and propagate rampdown to previous move
      //zprintf(PSTR("\nBACKPLANNNER TRD %d\n"), fi(m->rampdown));
      //zprintf(PSTR("RU:%d Rd:%d TS:%d\n "), fi(m->rampup), fi(m->rampdown), fi(ytotalstep));
      //zprintf(PSTR("FS:%f FN:%f FE:%f\n"), ff(m->fs), ff(m->fn), ff(m->fe));
#ifdef BACKPLANNER
      m->rampup = 0;
      m->rampdown = ytotalstep;
      m->fn = m->fs = speedat(m->fe, m->ac, ytotalstep, stepmm);
      CORELOOP
      int32_t ls = m->fs;
      tmove* mi;
      bpos = prevbuff(bpos);
      while (bpos != tail) {
        mi = &move[bpos];
        int32_t ts = labs(mi->dx[FASTAXIS(mi)]);
        mi->fe = ls;
        float stepmmi = Cstepmmx(FASTAXIS(mi));
        ramplen(mi->rampdown, mi->fe, mi->fs, mi->ac, stepmmi );
        CORELOOP
        if (mi->rampdown <= ts) {
          int32_t sub = ts - (mi->rampdown + mi->rampup);
          if (sub < 0) {
            mi->rampdown -= sub;
            mi->rampup -= sub; //fmax(0,m->rampup-sub);
          }
          mi->fn  = speedat(mi->fe, mi->ac, mi->rampdown, stepmmi);
          break;
        } else {
          mi->rampup = 0;
          mi->rampdown = ts;
          mi->fn = mi->fs = speedat(mi->fe, mi->ac, ts, stepmmi);
          ls  = mi->fs;
          bpos = prevbuff(bpos);
        }
      }
      CORELOOP
      if (bpos == tail) {
        zprintf(PSTR("\n\nReach tail!\n\n"));
      }

#else
      // just set the actual exit speed
      m->rampup = 0;
      m->fn = m->fs;
      m->rampdown = ytotalstep;

#endif
    } else {
      // calculate new nominal speed if up and down crossing
      m->fn = speedat(m->fs, m->ac, m->rampup, stepmm);
    }
  }


#ifdef preprampdebug
  zprintf(PSTR("========FINAL========\nRU:%d Rd:%d\n"), m->rampup, m->rampdown);
  zprintf(PSTR("FS:%f AC:%f FN:%f FE:%f\n"), ff(m->fs), ff(m->ac), ff(m->fn),  ff(m->fe));
#endif
  //if (m->fs < 1) m->fs = 1;
  m->status |= 4;
  CORELOOP
}

/*
    =================================================================================================================================================
    PLANNER
    =================================================================================================================================================
  dipanggil oleh   untuk mulai menghitung dan merencakanan gerakan terbaik
  kontrol mbelok adalah cara mengontrol supaya saat menikung, kecepatan dikurangi sehingga tidak skip motornya

*/

int currf[5], prevf[5];
void planner(int32_t h)
{
  // mengubah semua cross feedrate biar optimal secepat mungkin
  int32_t p;
  tmove *curr;
  tmove *prev;

  curr = &move[h];
  int32_t  scale = 1 << 4;
  int32_t xtotalstep = abs(curr->dx[FASTAXIS(curr)]);
  memcpy(prevf, currf, sizeof prevf);

  for (int i = 0; i < NUMAXIS; i++) {
    //prevf[i] = currf[i];
    currf[i] = 0;
    if (curr->dx[i] != 0) {
      int32_t scale2 = (maxf[i] << 4) * xtotalstep / abs(int32_t(curr->fn) * curr->dx[i]);
      if (scale2 < scale) scale = scale2;
      currf[i] = curr->dx[i] >> 6;

      CORELOOP
    }
  }
  // update all speed and square it up, after this all m->f are squared !
  /*currf[0] = (currf[0] * scale) >> 4;
    currf[1] = (currf[1] * scale) >> 4;
    currf[2] = (currf[2] * scale) >> 4;
    currf[3] = (currf[3] * scale) >> 4;
    currf[4] = (curr->fn * scale) >> 4;
  */
  scale *= curr->fn;
  curr->fn = (scale * scale) >> 8;
  //#define JERK1

#ifdef JERK1
#ifdef FASTDISTANCE
  prevdis = currdis;
  currdis = approx_distance_3(abs(currf[0]), abs(currf[1]), abs(currf[2]));
  CORELOOP
#endif
#endif
  if (bufflen() < 1) return;
  p = prevbuff(h);
  prev = &move[p];
  if ((prev->status & 3) == 2)return;// if already planned

  // ==================================
  // Centripetal corner max speed calculation, copy from other firmware
  if (prev->status & 3) {

    int32_t max_f = MINCORNERSPEED * MINCORNERSPEED;
    /*

         ======================================================
      change the junction speed to centripetal acceleration model
      junc cos = (prev.dx*cur.dx/(prev.totalstep*cur.totalstep) - (prev.dy ...


    */
#ifdef JERK1
#ifdef FASTDISTANCE
    int32_t divi = (prevdis * currdis);
#else
#define sqr2(n) n*n
    float divi = sqrt(sqr2(prevf[0]) + sqr2(prevf[1]));
    divi *= sqrt(sqr2(currf[0]) + sqr2(currf[1]));
    divi = 10000.f / divi ;
#endif
#ifdef output_enable
    zprintf (PSTR("DX[0]%d Divi :%f\n"), fi(curr->dx[0]), ff(divi));
#endif
    CORELOOP
    if (divi > 0) {
      //#define dot(ix) (prev->dx[ix]*curr->dx[ix])
#define dot(ix) (int32_t(prevf[ix])*int32_t(currf[ix]))

      int32_t junc_cos =  (dot(0) + dot(1) + dot(2)) * 10000 / divi;
      CORELOOP
#ifdef output_enable
      zprintf (PSTR("Cos :%f\n"), ff(junc_cos));
#endif

      if (junc_cos < 9999) {
        //junc_cos*=2;
        junc_cos = fmax(junc_cos, -9999); // Check for numerical round-off to avoid divide by zero.
        //zprintf (PSTR("Cos :%f\n"), ff(junc_cos));
        int32_t sin_theta_d2 = sqrt32((10000 - junc_cos) >> 1); // result will be range 0-500// Trig half angle identity. Always positive.
        CORELOOP
        //zprintf (PSTR("Sin :%f\n"), ff(sin_theta_d2));
        //zprintf (PSTR("Ac :%d\n"), fi(prev->ac));

        // TODO: Technically, the acceleration used in calculation needs to be limited by the minimum of the
        // two junctions. However, this shouldn't be a significant problem except in extreme circumstances.
        max_f = fmax(MINCORNERSPEED * MINCORNERSPEED, (sin_theta_d2 * DEVIATE *  (int32_t)curr->ac ) / ((100 -  sin_theta_d2)));
        CORELOOP

        //zprintf ("Cos :%f\n", ff(junc_cos ));
      } else max_f = 100000;
    };
    //if (max_f < 0)max_f = 9;
#ifdef output_enable
    zprintf (PSTR("MF:%d\n"), fi(max_f));
#endif
#elif JERK2

    int32_t fdx = currf[0] - prevf[0];
    int32_t fdy = currf[1] - prevf[1];
    int32_t fdz = currf[2] - prevf[2];

    int32_t jerk = sqrt32(fdx * fdx + fdy * fdy) ;
    CORELOOP

    max_f = fmin(currf[4], prevf[4]);
    if (jerk > XYJERK) max_f = XYJERK * max_f / jerk;
    jerk = abs(fdz) ;
    if (jerk > ZJERK) max_f = ZJERK * max_f / jerk;

    max_f = fmax(max_f, MINCORNERSPEED);
    CORELOOP
#ifdef output_enable
    zprintf (PSTR("JERK:%d\n"), fi(jerk));
    zprintf (PSTR("XMF:%d\n"), fi(max_f));
#endif
    max_f *= max_f;

#else// jerk2
    max_f = 10000;
    for (int i = 0; i < 3; i++) {
      int32_t jerk = abs(currf[i] - prevf[i]);
      if (jerk > XYJERK) max_f = fmin(max_f, (prevf[i] + currf[i]) / 2 + XYJERK);
      //else if (jerk > XYJERK) max_f = fmin(max_f, prevf[0] - XYJERK);
      CORELOOP
    }
    max_f = fmax(max_f, MINCORNERSPEED);
#ifdef output_enable
    zprintf (PSTR("XMF:%d\n"), fi(max_f));
#endif
    max_f *= max_f;
#endif

    prev->fe = fmin(max_f, prev->fn);
    prev->fe = fmin(curr->fn, prev->fe);
    //zprintf (PSTR("PF:%d\n"), fi(prev->fe));
    prepareramp(p);
    curr->fs = prev->fe;
    CORELOOP
  }

}

/*
    =================================================================================================================================================
    ADDMOVE
    =================================================================================================================================================
  Rutin menambahkan sebuah vektor ke dalam buffer gerakan
*/
int32_t x1[NUMAXIS], x2[NUMAXIS];
tmove *m, *wm;
#ifdef ISPC
float fctr, tick;
float tickscale, fscale, graphscale;
#endif
//int32_t px[4] = {0, 0, 0, 0 };
#ifdef DRIVE_DELTA
float rampv;
float istepmmx[4];
#else
#define rampv 1
#endif

int32_t  f;
#ifdef DRIVE_DELTA
float rampseg, rampup, rampdown;
#else
int32_t rampseg, rampup, rampdown;
#endif
int32_t ta, acup, acdn, oacup, oacdn;
int32_t mctr, xtotalseg;
uint8_t fastaxis;
uint32_t nextmicros;
uint32_t nextdly;
uint32_t nextmotoroff;


void addmove(float cf, float cx2, float cy2 , float cz2, float ce02, int g0 , int rel)
{

  cf *= f_multiplier;
  float x2[4];
#ifdef __AVR__
  if (cf > 120)cf = 120; // prevent max speed
#endif

  if (head == tail) {
    //zprintf(PSTR("Empty !\n"));
  }
#ifdef output_enable
  xprintf(PSTR("Tail:%d Head:%d \n"), fi(tail), fi(head));
  xprintf(PSTR("F:%f From X:%f Y:%f Z:%f\n"), ff(cf), ff(cx1), ff(cy1), ff(cz1));
  xprintf(PSTR("To X:%f Y:%f Z:%f\n"),  ff(cx2), ff(cy2), ff(cz2));
#endif
  //zprintf(PSTR("X:%f Y:%f Z:%f\n"), ff(cx2), ff(cy2), ff(cz2));
  //zprintf(PSTR("-\n"));
  needbuffer();
  //cf = 30;
  tmove *curr;
  curr = &move[nextbuff(head)];
  curr->status = g0 ? 8 : 0; // reset status:0 planstatus:0 g0:g0
  //curr->g0 = g0;
#ifdef ISPC
  curr->col = 2 + (head % 4) * 4;
  //curr->xs[0] = cx1 * Cstepmmx(0);
  //curr->xs[1] = cy1 * Cstepmmx(1);
  //curr->xs[2] = cz1 * Cstepmmx(2);
#endif

  if (rel) {
    cx1 = cy1 = cz1 = ce01 = 0;
  }

#ifdef ISPC
  curr->col = 2 + (head % 4) * 4;
#endif

  x2[0] = (int32_t)(cx2 * Cstepmmx(0)) - (int32_t)(cx1 * Cstepmmx(0))  ;
  x2[1] = (int32_t)(cy2 * Cstepmmx(1)) - (int32_t)(cy1 * Cstepmmx(1)) ;
  x2[2] = (int32_t)(cz2 * Cstepmmx(2)) - (int32_t)(cz1 * Cstepmmx(2)) ;
  CORELOOP

  x2[3] = (ce02 - ce01) * stepmmx[3]  * e_multiplier;
#if defined( DRIVE_COREXY)
  // 2 = z axis + xy H-gantry (x_motor = x+y, y_motor = y-x)
  // borrow cx1,cy1 for calculation
  cx1 = x2[0] + x2[1];
  cy1 = x2[1] - x2[0];
  x2[0] = cx1;
  x2[1] = cy1;

#elif defined(DRIVE_COREXZ)
  // 8 = y axis + xz H-gantry (x_motor = x+z, z_motor = x-z)
  cx1 = x2[0] + x2[2];
  cy1 = x2[0] - x2[2];
  x2[0] = cx1;
  x2[2] = cy1;

#elif defined(DRIVE_DELTA)


#endif
  CORELOOP
  curr->fn = cf; //curr->fn *= curr->fn;
  curr->fe = 0;
  curr->fs = 0;
  //curr->planstatus = 0; //0: not optimized 1:fixed
  //calculate delta
  int32_t dd;
  dd = 0;
  int faxis = 0;
  // calculate the delta, and direction
  for (int i = 0; i < NUMAXIS; i++) {
#define delta  x2[i]
    curr->dx[i] = delta;
    delta = abs(delta);
    if (delta > dd) {
      dd = delta;
      faxis = i;
    }
  }
  // if zero length then cancel
  if (dd < MINIMUMSTEPS)
  {
#ifdef output_enable
    zprintf(PSTR("Step:%d F:%f A:%d\n"), fi(dd), ff(cf), fi(curr->ac));
#endif
  } else
  {
#ifdef output_enable
    xprintf(PSTR("Totalstep AX%d %d\n"), (int32_t)faxis, (int32_t)curr->dx[faxis]);
#endif
    curr->status |= faxis << 4;
    curr->ac = 2 * (g0 ? mvaccel : accel);
    //zprintf(PSTR("F:%f A:%d\n"), ff(cf), fi(curr->ac));



    // back planner
    head = nextbuff(head);
    curr->status |= 1; // 0: finish 1:ready
    // planner are based on cartesian coord movement on the motor
    planner(head);
#ifdef DRIVE_DELTA
    cx1 = curr->dtx[0] = cx2; // save the target, not the original
    cy1 = curr->dtx[1] = cy2;
    cz1 = curr->dtx[2] = cz2;
#else
    cx1 = cx2;
    cy1 = cy2;
    cz1 = cz2;
#endif
    ce01 = ce02;
  }
  zprintf(PSTR("\n"));

}


#ifndef DRIVE_DELTA
#define DELTA_TEST
#else
#define sqrtstep(x) stepmmx[0]* sqrt(x)
// maybe we should use fixed point to increase performance, or make all already multiplied with the steppermm (steppermm as the pixed point multiplicator)
void transformdelta( float x, float y, float z) {
  //#define DELTA_TEST
#ifndef DELTA_TEST

#ifdef ISPC
  float ex = x * graphscale;
  float ey = y * graphscale;
  float ez = z * graphscale;

  putpixel (ex + ey * 0.3 + 250, ey * 0.3 - ez + 250, 15);
#endif
  x2[0]     = stepmmx[0] * (sqrt(DELTA_DIAGONAL_ROD_2
                                 - (delta_tower1_x - x) * (delta_tower1_x - x)
                                 - (delta_tower1_y - y) * (delta_tower1_y - y)
                                ) + z);
  x2[1]     = stepmmx[1] * (sqrt(DELTA_DIAGONAL_ROD_2
                                 - (delta_tower2_x - x) * (delta_tower2_x - x)
                                 - (delta_tower2_y - y) * (delta_tower2_y - y)
                                ) + z);
  x2[2]     = stepmmx[2] * (sqrt(DELTA_DIAGONAL_ROD_2
                                 - (delta_tower3_x - x) * (delta_tower3_x - x)
                                 - (delta_tower3_y - y) * (delta_tower3_y - y)
                                ) + z);
#else
  // test segmentation work
  x2[0] = int32_t(stepmmx[0] * x);
  x2[1] = int32_t(stepmmx[1] * y);
  x2[2] = int32_t(stepmmx[2] * z);
#endif
#ifdef output_enable
  zprintf(PSTR("transform delta : %f %f %f -> %d %d %d\n"), ff(x), ff(y), ff(z), fi(x2[0]), fi(x2[1]), fi(x2[2]));
#endif
}

float xc[3];
int32_t ctotalseg, ctotalstep, cbsdx[3];
int8_t csx[3];

// this part must calculated in parts to prevent single long delay
void calculate_delta_steps(int s) {
#ifdef ISPC
  zprintf(PSTR("\nSEG PRECALC %d\n"), s);
#endif
  switch (s) {

    case 4:
      // STEP 1
      ctotalseg = xtotalseg - 1;
      memcpy(x1, x2, sizeof x1);
      memcpy(xc, m->dtx, sizeof xc);
      // STEP 2
      break;
    case 3:
      for (int i = 0; i < 3; i++) {
        xc[i] -= ctotalseg * sgx[i];
      }
      break;
    case 2:
      // STEP 3
      transformdelta(xc[0], xc[1], xc[2]);
      break;
    case 1:
      // STEP 4
      // ready for movement
      ctotalstep = bsdx[3];
      for (int i = 0; i < 3; i++) {
        int32_t d = x2[i] - x1[i];
        if (d < 0) {
          cbsdx[i] = -d;
          csx[i] = -1;
        } else {
          cbsdx[i] = d;
          csx[i] = 1;
        }
        ctotalstep = fmax(cbsdx[i], ctotalstep);
      }
      // modify the sx?? m->mcx and totalstep for this segment
      break;
    case 0:
      // STEP 5 FINAL
      memcpy(sx, csx, sizeof csx);
      memcpy(bsdx, cbsdx, sizeof cbsdx);
      xtotalseg--;
      mcx[0] = mcx[1] = mcx[2] = mcx[3] = 0;
      mctr = totalstep = ctotalstep;
      rampv = rampseg / mctr;

      acup = oacup * rampv;
      acdn = oacdn * rampv;
      motor_0_DIR(sx[0]);
      motor_1_DIR(sx[1]);
      motor_2_DIR(sx[2]);
      motor_3_DIR(sx[3]);
      pinCommit();
      dobacklash();
#ifdef output_enable
      zprintf(PSTR("\nX:%d %d %d \n"), fi(bsdx[0]), fi(bsdx[1]), fi(bsdx[2]));
      zprintf(PSTR("Segm:%d Step:%d SX:%d %d %d\n"), fi(xtotalseg), fi(totalstep), fi(sx[0]), fi(sx[1]), fi(sx[2]));
#endif
#ifdef ISPC
      m->col++;
#endif
  }
}
#endif


/*
    =================================================================================================================================================
    MOTIONLOOP
    =================================================================================================================================================
*/
void otherloop(int r);

float xs[4] = {0, 0, 0, 0};
float gx, gy, lx, ly;
uint32_t cm, ocm,  mctr2, dlmin, dlmax;
int32_t timing = 0;
int coreloop1() {
  if (!m || (mctr <= 0))return 0;

#ifndef USETIMER1
#ifdef ISPC
  if (1) {
#else
  //cm = micros();
  if (cm - nextmicros >= dl) { //10 is average miss
#ifdef steptiming
    timing = fmax(timing, (cm - nextmicros) - dl);
#endif
    nextmicros = cm;

#endif
#else // not USETIMER1
  {
#endif

#ifdef ISPC
    //f = dl/20;//
    f = stepdiv / (float)dl;
    tick = tick + dl; //1.0/timescale;
    int32_t c = m->col & 7 + 8;

    //float cf = float(timescale) / dl;

    //pset (tick*tickscale+10,400-f*fscale),c
#define realpos1(i) (xs[i] / stepmmx[i])
    if (tick * tickscale / timescale > 600) {
      gx = 0;
      tick = 0;
    }
    //putpixel (tick * tickscale / timescale + 30, 450 -  fscale * cf, c - 8);
    lx = gx;
    ly = gy;
    gx = tick * tickscale / timescale + 10;
    gy = 480 -  fscale * f;
    setcolor(c);
    //putpixel (gx, gy, c);
    line(lx, ly, gx, gy);
    //zprintf(PSTR("X:%f Y:%f\n"), ff(gx), ff(gy));
    setcolor(0);
    line(gx + 1, 480 - 1, gx + 1, 480 - 100 * fscale);

#ifndef DELTA_TEST
    float ex = realpos1(0) * graphscale;
    float ey = realpos1(1) * graphscale;
    float ez = realpos1(2) * graphscale;

    putpixel (150, 251 - ex + 0, 0);
    putpixel (350, 251 - ey + 0, 0);
    putpixel (250, 201 - ez + 0, 0);
    putpixel (150, 250 - ex + 0, 9);
    putpixel (350, 250 - ey + 0, 12);
    putpixel (250, 200 - ez + 0, 14);
#else
    float ex = realpos1(0) * graphscale;
    float ey = realpos1(1) * graphscale;
    float ez = realpos1(2) * graphscale;

    //putpixel (ex + 150, ey + 150, c);
    putpixel (ex + ey * 0.3 + 320, ey * 0.3 - ez + 150, c);
#endif
    //zprintf(PSTR("%f\n"), cf);
    //pset (x(1)/stepmmx(1)+50,x(2)/stepmmx(2)+40),c
#define graphstep(ix) xs[ix] +=sx[ix]
#else // ispc
#define graphstep(ix)
#endif
    /* ================================================================================================================
                                                    BRESENHAM CORE

                                  this need to move to interrupt for smooth movement
       ================================================================================================================
    */

#define bresenham(ix)\
  if ((mcx[ix] -= bsdx[ix]) < 0) {\
    motor_##ix##_STEP();\
    mcx[ix] += totalstep;\
    graphstep(ix); \
  }

    // without DIO min 64us, with DIO can 44us

    bresenham(0);
    bresenham(1);
    bresenham(2);
    bresenham(3)
    pinCommit();
    STEPDELAY

    motor_0_UNSTEP();
    motor_1_UNSTEP();
    motor_2_UNSTEP();
    motor_3_UNSTEP();
    pinCommit();
    // next speed
#ifdef ISPC
    fctr += rampv;
#endif
    if ((rampup -= rampv) > 0) {
      ta += acup;
      goto UPDATEDELAY;
    }
    else if ((rampdown -= rampv) < 0) {
      ta += acdn;
      // calculate new delay only if we accelerating
UPDATEDELAY:
      // if G0 update delay not every step to make the motor move faster
      //if (m->status & 8) {
#ifdef UPDATE_F_EVERY
      nextdly += dl;
      if (nextdly > UPDATE_F_EVERY)
      {
        nextdly -= UPDATE_F_EVERY;
        dlp = xInvSqrt(ta);
      };
#else
      dlp = xInvSqrt(ta);
#endif
      // if G1, update every time
      //} else dl = xInvSqrt(ta);
    }
#ifdef ISPC

#endif
#ifdef INTERPOLATEDELAY
#define CALCDELAY dl = ((dl<<4)+(dlp-dl))>>4; // hack, linear interpolation of the delay
    //#define CALCDELAY dl = (dlp+dl)>>1; // hack, linear interpolation of the delay
#else
#define CALCDELAY dl = dlp;
#endif

    CALCDELAY

    mctr--;
    if (mctr  == 0)zprintf(PSTR("\n"));
#ifdef output_enable
    //zprintf(PSTR("%d "), dl);
    //zprintf(PSTR("%d "), mctr);
#endif
    /* ================================================================================================================ */

#ifdef output_enable
    // write real speed too
    dlmin = fmin(dl, dlmin);
    dlmax = fmax(dl, dlmax);
    if (mctr2++ % 20 == 0)
    {
      //zprintf(PSTR("RU:%f RD:%f "), ff(rampup), ff(rampdown));
      //printf(PSTR("D:%d RD:%f "), ff(rampup), ff(rampdown));
      //zprintf(PSTR("dmin:%d dmax:%d "), fi(dlmin), fi(dlmax));
      //zprintf(PSTR("Fmin:%d Fmax:%d\n"), fi(stepdiv / dlmax), fi(stepdiv / dlmin));
      //zprintf(PSTR("F:%d\n"), fi(stepdiv/dlmin));

      dlmin = 1000000;
      dlmax = 0;
    }
#endif
#ifndef ISPC
    //if (mctr % 10 == 0)zprintf(PSTR("%d "), fi(mctr));
    //if (mctr % 4 == 0) zprintf(PSTR("F:%d "), fi(stepdiv/dl));
    //if (mctr2++ % 20 == 0) Serial.println("");
#endif
    //    if (mctr % 10 == 0)zprintf(PSTR("endstp %d\n"),fi(checkendstop));
    if (checkendstop) {
      docheckendstop();
      if ((endstopstatus[0] < 0) || (endstopstatus[1] < 0) || (endstopstatus[2] < 0)) {
        zprintf(PSTR("Endstop hit"));
        m->status = 0;
        mctr = 0;
        m = 0;
        wm = 0;
        return 0;
      }
    }
    return 1;
  }
  return 0;
}

int busy = 0;
int motionloop() {
  int  r;
#ifndef USETIMER1
  if (busy)return 0;
  busy = 1;
  cm = micros();
  if (!m ) r = startmove(); else
#endif
  {
    // for auto start motion when idle
    // main loop
    r = coreloop1();
    //if (mctr)
    timer_set(dl);
#ifndef USETIMER1
  }
  otherloop(r);
  busy = 0;
#else
  }
#endif //USETIMER1  
  return r;
}

void otherloop(int r) {
#ifdef USETIMER1
  cm = micros();
#endif

  // NON TIMER
#ifndef USETIMER1
  if (r && m && (mctr < 6)) {
    if (mctr == 0) {

#ifdef DRIVE_DELTA
      // delta need to check if totalseg
      if (xtotalseg > 0) {
        // next segment
        // calculate next bresenham
        calculate_delta_steps(0);
      } else
#endif //delta
      {

#ifdef steptiming
        zprintf(PSTR("Max miss %dus \n"), fi(timing));
        timing = 0;
#endif
        // coreloop ended, check if there is still move ahead in the buffer
        //zprintf(PSTR("Finish:%d %f\n"), fi(mctr2), ff(fctr));
        m->status = 0;
        if (m->fe == 0)f = 0;
        m = 0;
NEWMOVE:
        r = startmove();
      }
    }
    else {
#ifdef DRIVE_DELTA
      if (xtotalseg > 0)calculate_delta_steps(mctr); else r = startmove();
#else
      r = startmove();
#endif

    }
  }
#else //ifndef USETIMER1  
  // TIMER ===================================================
  if (!m) startmove();
  if (m && (mctr < 6)) {
    if (mctr == 0) {

#ifdef DRIVE_DELTA
      // delta need to check if totalseg
      if (xtotalseg > 0) {
      } else
#endif //delta
      {
        // coreloop ended, check if there is still move ahead in the buffer
        //zprintf(PSTR("Finish:%d %f\n"), fi(mctr2), ff(fctr));

        m->status = 0;
        if (m->fe == 0)f = 0;
        m = 0;
        //startmove();
      }
    }
    else {
      // call preparation early 15 steps, i think its enough
#ifdef DRIVE_DELTA
      if (xtotalseg > 0)calculate_delta_steps(); else r = startmove();
#else
      r = startmove();
#endif

    }
  }

#endif
#ifndef ISPC

  // this both check take 8us
  temp_loop(cm);
  if (!m   && (cm - nextmotoroff >= motortimeout)) {
    //xprintf(PSTR("Motor off\n"));
    nextmotoroff = cm;
    power_off();
  }
#endif
#if defined(ESP8266)
  feedthedog();
#endif
}

/*
    =================================================================================================================================================
    STARTMOVE
    =================================================================================================================================================
  Mulai menjalankan 1 unit di buffer gerakan terakhir
*/
int laxis;

int wt;
int wfastaxis;
int32_t wbsdx3, wsx3, wacup, wacdn, wtotalstep;
int32_t wbsdx[4];
int32_t wta, wdlp;
float ts, wstepdiv, wstepdiv2;
int wxtotalseg;
void startmovestep(int s);

int32_t startmove()
{
  if ((head == tail)) {
    //Serial.println("?");
    //m = 0; wm = 0; mctr = 0;
    return 0;
  }
#ifdef USETIMER1
  if (m && (mctr < 1)) {
    //if ((mctr>0) && (nextbuff(tail)==head)) return 0;
    startmovestep(mctr);
  }
#else
  if (wm || (m && (mctr == 5))) {
    startmovestep(mctr);
    return !mctr;
  }
#endif
  // last calculation
  if (m ) return 0; // if empty then exit
  // if no precalculation, then calculate all
  if (!wm) {
    startmovestep(-1);
  }
  return 1;
}

// break start move in some 5 steps to prevent long delay
void startmovestep(int s)
{
#ifdef USETIMER1
#define STEPCASE(n) //zprintf(PSTR("Step %d\n"),fi(n));
#define STEPBREAK
  {
#else // TIMER1
#define STEPCASE(n) case n:    //zprintf(PSTR("\nStep ##n##\n"));

#define STEPBREAK if(s>-1)break;

  switch (s) {
    case -1:
      zprintf(PSTR("\nPRECALC %d\n"), fi(s));
      //      getch();

#endif
    STEPCASE(5)
    // STEP 1
    wt = nextbuff(tail);
    // start bresenham variable
    //zprintf(PSTR("Fastaxis:%d\n"),fi(fastaxis));
    // prepare ramp (for last move)
    prepareramp(wt);
    wm = &move[wt];

    wfastaxis = FASTAXIS(wm);
    wtotalstep = abs(wm->dx[wfastaxis]);


    STEPBREAK
    STEPCASE(4)
    // recalculate acceleration for up and down, to minimize rounding error
    wacup = wacdn = 0;
    if (wm->rampup)wacup = ((int32_t)(wm->fn  - wm->fs ) << INVSCALE ) / (float)wm->rampup;
    if (wm->rampdown)wacdn = -((int32_t)(wm->fn  - wm->fe ) << INVSCALE ) / (float)wm->rampdown;
    STEPBREAK

#ifdef DRIVE_DELTA
    // DELTA HERE
    // calculate first movement
    //STEP
    STEPCASE(3)

    wxtotalseg = 1 + (wtotalstep / 100);
    wbsdx3 = abs((wm->dx[3]) / wxtotalseg);
    wsx3 = wm->dx[3] < 0 ? -1 : 1;
    ts = 1.f / (wxtotalseg);
    rampseg = (float)(wtotalstep) * ts;
    ts *= IFIXED2;

    // STEP
    transformdelta(wm->dtx[0] - wm->dx[0]*IFIXED2, wm->dtx[1] - wm->dx[1]*IFIXED2, wm->dtx[2] - wm->dx[2]*IFIXED2);

    // in the last 4 steps, we  sure the previous move is not using this anymore
    oacup = wacup;
    oacdn = wacdn;

    sgx[0] = float(wm->dx[0]) * ts;
    sgx[1] = float(wm->dx[1]) * ts;
    sgx[2] = float(wm->dx[2]) * ts;
#ifdef ISPC
    zprintf(PSTR("\nTotalSeg:%d RampSeg:%f Sx:%f Sy:%f Sz:%f\n"), fi(wxtotalseg), ff(rampseg), ff(sgx[0]), ff(sgx[1]), ff(sgx[2]));
    zprintf(PSTR("Dx:%d Dy:%d Dz:%d\n"), fi(wm->dtx[0]), fi(wm->dtx[1]), fi(wm->dtx[2]));
#endif
    STEPBREAK
    STEPCASE(2)
#endif

    STEPBREAK
    STEPCASE(1)
    // STEP
    wta = (int32_t)(wm->fs ) << INVSCALE ;
    wstepdiv = CLOCKCONSTANT / (stepmmx[wfastaxis]);
    wstepdiv2 = wstepdiv * INVSCALE2;
    wdlp = xInvSqrt2(wta, wstepdiv, wstepdiv2);
    wm->status &= ~3;
    wm->status |= 2;
    STEPBREAK
    STEPCASE(0)
    // FINAL ?
#ifdef USETIMER1
    // wait until mctr = 0
    //zprintf(PSTR("C0\n"));
    while (mctr) {
      //SEI
      delayMicroseconds(20);
      //CLI
    }
#endif
    CLI
    m = wm;
    wm = 0;
    laxis = fastaxis;
    fastaxis = wfastaxis;
    acup = wacup;
    acdn = wacdn;
    ta = wta;
    stepdiv = wstepdiv;
    stepdiv2 = wstepdiv2;
    dlp = wdlp;
    if (stepmmx[laxis] != stepmmx[fastaxis])
      dl = dlp;//*stepmmx[laxis]/stepmmx[fastaxis]; // interpolate continue from last move, if same fastaxis
    if (f == 0)
      nextmicros = micros();// if from stop

    rampup = m->rampup  * SUBMOTION;
    rampdown = wtotalstep - rampup - m->rampdown * SUBMOTION;
    mctr2 = mcx[0] = mcx[1] = mcx[2] = mcx[3] = 0; //mctr >> 1;
    tail = wt;

#ifdef DRIVE_DELTA
    // i dont know how to break this because previous move still running and they still use the variables. especially the totalseg value
    sx[3] = wsx3;
    bsdx[3] = wbsdx3;
    xtotalseg = wxtotalseg;
    calculate_delta_steps(4);
    calculate_delta_steps(3);
    calculate_delta_steps(2);
    calculate_delta_steps(1);
    calculate_delta_steps(0);
#else
    for (int i = 0; i < 4; i++) {
      if (m->dx[i] > 0) {
        bsdx[i] = (m->dx[i]);
        sx[i] = 1;
      } else {
        bsdx[i] = -(m->dx[i]);
        sx[i] = -1;
      }
    }
    motor_0_DIR(sx[0]);
    motor_1_DIR(sx[1]);
    motor_2_DIR(sx[2]);
    motor_3_DIR(sx[3]);
    pinCommit();
    dobacklash();
#ifdef AUTO_MOTOR_Z_OFF
    if (bsdx[2] == 0)motor_2_OFF();
#endif // uato motor off

    totalstep = mctr = wtotalstep * SUBMOTION;
#endif


    SEI
    timer_set(dl);

#ifdef ISPC
    fctr = 0;
#endif

#ifdef output_enable
    zprintf(PSTR("Start tail:%d head:%d\n"), fi(tail), fi(head));
    zprintf(PSTR("RU:%d Rd:%d Ts:%d\n"), m->rampup, m->rampdown, fi(mctr));
    zprintf(PSTR("FS:%f FN:%f FE:%f AC:%f \n"), ff(m->fs), ff(m->fn), ff(m->fe), ff(m->ac));
    zprintf(PSTR("TA,ACX:%d,%d \n"), fi(ta), fi(acup));
    zprintf(PSTR("DX:%d DY:%d DZ:%d DE:%d \n"), fi(m->dx[0]), fi(m->dx[1]), fi(m->dx[2]), fi(m->dx[3]));
    //xprintf(PSTR("Last %f %f %f \n"), ff(px[0] / stepmmx[0]), ff(px[1] / stepmmx[0]), ff(px[2] / stepmmx[0]));
    //xprintf(PSTR("sx %d %d %d \n"), fi(sx[0]), fi(sx[1]), fi(sx[2]));
    //xprintf(PSTR("Status:%d \n"), fi(m->status));

#endif
  }
}

/*
    =================================================================================================================================================
    DOCHECKENDSTOP
    =================================================================================================================================================
*/
#ifdef INVERTENDSTOP
#define ENDCHECK !
#else
#define ENDCHECK
#endif

#define ENDPIN INPUT_PULLUP

void docheckendstop()
{
  // AVR specific code here
  // read end stop
#ifdef xmin_pin
  endstopstatus[0] = xmin_pin;
#elif defined(xmax_pin)
  endstopstatus[0] = xmax_pin;
#else
  endstopstatus[0] = 0;
#endif
#ifdef ymin_pin
  endstopstatus[1] = ymin_pin;
#elif defined(ymax_pin)
  endstopstatus[1] = ymax_pin;
#else
  endstopstatus[1] = 0;
#endif
#ifdef zmin_pin
  endstopstatus[2] = zmin_pin;
#elif defined(zmax_pin)
  endstopstatus[2] = zmax_pin;
#else
  endstopstatus[2] = 0;
#endif



#ifndef ISPC
  for (int e = 0; e < 3; e++) {
    if (endstopstatus[e]) {
      int nc = 0;
      for (int d = 0; d < 5; d++) {
        if (ENDCHECK digitalRead(endstopstatus[e]))nc++;
      }
      if (nc > 3)endstopstatus[e] = -1;
    }
  }
#else
  // PC
  if (!m) return;
  // simulate endstop if x y z is 0
  for (int32_t e = 0; e < NUMAXIS; e++) {
    if (x1[e] < 0) endstopstatus[e] < 0; else  endstopstatus[e] = 0;
  }
#endif
}

/*
    =================================================================================================================================================
    HOMING
    =================================================================================================================================================
*/
void homing()
{
  // clear buffer
  waitbufferempty();
  int32_t tx[4];
  tx[0] =  tx[1] = tx[2] = 0;
#define mmax ENDSTOP_MOVE*100
#ifdef xmin_pin
  tx[0] = -mmax;
#elif defined(xmax_pin)
  tx[0] = mmax;
#endif

#ifdef ymin_pin
  tx[1] = -mmax;
#elif defined(ymax_pin)
  tx[1] = mmax;
#endif
#ifdef zmin_pin
  tx[2] = -mmax;
#elif defined(zmax_pin)
  tx[2] = mmax;
#endif
  checkendstop = 1;
  //xprintf(PSTR("Homing to %d %d %d\n"), tx[0], tx[1], tx[2]);
  addmove(homingspeed, tx[0], tx[1], tx[2], ce01);

  // now slow down and check endstop once again
  waitbufferempty();
  //docheckendstop();
  float xx[4];
  //xprintf(PSTR("ENDSTOP %d %d %d\n"), (int32_t)endstopstatus[0], (int32_t)endstopstatus[1], (int32_t)endstopstatus[2]);
  //xprintf(PSTR("Position %f %f %f \n"), ff(cx1), ff(cy1), ff(cz1));
  // move away from endstop
#define moveaway(e,F) {\
    if (tx[e]) {\
      xx[0]=xx[1]=xx[2]=xx[3]=0;\
      xx[e] =  - tx[e]/100;\
      CLI checkendstop = 0;\
      addmove(F, xx[0], xx[1], xx[2], 0,1,1);\
      waitbufferempty();\
    }\
  }
#define xcheckendstop(e,F) {\
    xx[0]=xx[1]=xx[2]=0;\
    xx[e] = tx[e];\
    addmove(F, xx[0], xx[1], xx[2], 0,1,1);\
    CLI checkendstop = 1;\
    waitbufferempty();\
    xx[e] =  - tx[e]/100;\
    addmove(F, xx[0], xx[1], xx[2], 0,1,1);\
    CLI checkendstop = 0;\
    waitbufferempty();\
  }
  for (int32_t e = 0; e < 3; e++) {
    moveaway(e, homingspeed);
  }
  for (int32_t e = 0; e < 3; e++) {
    if (tx[e]) {
      // check endstop again fast
      xcheckendstop(e, homingspeed);
      xcheckendstop(e, homingspeed / 10);
    }
  }
  checkendstop = ce01 = 0;

#ifdef xmax_pin
  cx1 = ax_max[0] * 0.1;
#else
  cx1 = 0;
#endif

#ifdef ymax_pin
  cy1 = ax_max[1] * 0.1;
#else
  cy1  = 0;
#endif

#ifdef zmax_pin
  cz1 = ax_max[2] * 0.1;
#else
  cz1 = 0;
#endif
  ce01 = 0;
  //xprintf(PSTR("Home to:X:%f Y:%f Z:%f\n"),  ff(cx1), ff(cy1), ff(cz1));

}

/*
    =================================================================================================================================================
    WAITBUFFEREMPTY
    =================================================================================================================================================
  waitbufferempty
  Proses semua gerakan di buffer, dan akhiri dengan deselerasi ke 0
*/
void waitbufferempty()
{
  prepareramp(head);
  startmove();
  zprintf(PSTR("Wait %d\n"), fi(mctr));
  int otail = tail;
  while ((head != tail) || m) //(tail == otail) //
  {
    SEI
    domotionloop
    //delayMicroseconds(20);
    //zprintf(PSTR("Wa\n"));
    //CLI
  }
  zprintf(PSTR("Empty"));

}
/*
    =================================================================================================================================================
    NEEDBUFFER
    =================================================================================================================================================
  loop sampai da buffer yang bebas
*/
void needbuffer()
{
  if (nextbuff(head) == tail) {
#ifdef output_enable
    xprintf(PSTR("Wait %d / %d \n"), fi(tail), fi(head));
#endif
    //wait current move finish
    int t = tail;
    while (t == tail) {
      SEI
      domotionloop
      //delayMicroseconds(20);
      //zprintf(PSTR("Nb\n"));
      //CLI
    }
    //xprintf(PSTR("Done\n"));
  }
#ifdef output_enable
  xprintf(PSTR("Done %d / %d \n"), fi(tail), fi(head));
#endif
}
/*
    =================================================================================================================================================
    initmotion
    =================================================================================================================================================
  inisialisasi awal, wajib
*/
void initmotion() {
#ifdef USETIMER1
  timer_init();
#endif
  reset_motion();
  preparecalc();
  dl = 5000;
  wm = 0;

#ifdef ISPC
  tickscale = 60;
  fscale = 2;
  graphscale = 4;
#endif
  motor_0_INIT();
  motor_1_INIT();
  motor_2_INIT();
  motor_3_INIT();

#ifndef ISPC
#ifdef xmin_pin
  pinMode(xmin_pin, ENDPIN);
#endif
#ifdef xmax_pin
  pinMode(xmax_pin, ENDPIN);
#endif

#ifdef ymin_pin
  pinMode(ymin_pin, ENDPIN);
#endif
#ifdef ymax_pin
  pinMode(ymax_pin, ENDPIN);
#endif

#ifdef zmin_pin
  pinMode(zmin_pin, ENDPIN);
#endif
#ifdef zmax_pin
  pinMode(zmax_pin, ENDPIN);
#endif
  pinMotorInit
#endif
}


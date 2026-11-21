/* io.h falso para o simulador no PC */
#ifndef FAKE_IO_H
#define FAKE_IO_H
#include <stdint.h>
extern volatile uint8_t DDRB, PORTB, PINB, DDRC, PORTC, DDRD, PORTD, PIND;
extern volatile uint8_t SPCR, UCSR0A, UCSR0B, UCSR0C, EICRA, EIMSK, EIFR;
extern volatile uint8_t ADMUX, ADCSRA, ADCSRB, ADCH, OCR0A, OCR0B, TCCR0A, TCCR0B;
extern volatile uint8_t TIMSK0, TCNT0, TCCR2A, TCCR2B, OCR2A, TIMSK2, SREG;
extern volatile uint16_t UBRR0;
uint16_t *sim_spdr(void);
uint8_t *sim_spsr(void);
uint16_t *sim_udr(void);
#define SPDR (*sim_spdr())
#define SPSR (*sim_spsr())
#define UDR0 (*sim_udr())
enum { PB0, PB1, PB2, PB3, PB4, PB5, PB6, PB7 };
enum { PC0, PC1, PC2, PC3, PC4, PC5, PC6 };
enum { PD0, PD1, PD2, PD3, PD4, PD5, PD6, PD7 };
#define SPE 6
#define DORD 5
#define MSTR 4
#define SPIF 7
#define SPI2X 0
#define RXCIE0 7
#define RXEN0 4
#define TXEN0 3
#define UCSZ01 2
#define UCSZ00 1
#define UDRE0 5
#define ISC01 1
#define INT0 0
#define INTF0 0
#define REFS0 6
#define ADLAR 5
#define MUX2 2
#define MUX1 1
#define MUX0 0
#define ADEN 7
#define ADSC 6
#define ADATE 5
#define ADIE 3
#define ADPS2 2
#define ADPS1 1
#define ADPS0 0
#define ADTS2 2
#define COM0B1 5
#define COM0B0 4
#define WGM01 1
#define WGM00 0
#define WGM02 3
#define CS01 1
#define CS00 0
#define TOIE0 0
#define WGM21 1
#define CS22 2
#define OCIE2A 1
#endif

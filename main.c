/* 
 * File:   main.c
 * Author: thassio lima
 *
 * Created on February 9, 2025, 10:07 PM
 */
#define F_CPU 8000000

#include <stdio.h>
#include <stdlib.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include "font.h"
#include <string.h>


void string(void);
void print(void);
void send(void);
void USART_Transmit(void);

volatile uint8_t cont=0; //numero step
volatile uint8_t step[8]= {0b00001010,0b00000010,0b00000110,0b00000100,0b00000101,0b00000001,0b00001001,0b00001000};
volatile uint16_t i=0;    
    /*
                 0b00001000,
				 0b00001001,
				 0b00000001,
				 0b00000101,
				 0b00000100,
				 0b00000110,
				 0b00000010,
				 0b00001010};*/
 char buffer[500];// = "!Frase de teste com quebra de linha";
 char frase[100];

uint16_t tam=0; //tamanho da string
uint8_t linhas;
uint8_t li=0;
uint8_t pos=0;
uint8_t linha=0;
uint8_t lat=0; //verificar quantidade de lat

  uint16_t offset;
uint8_t L=0; //linha matriz caracter
uint8_t n_lat=0;


ISR(TIMER0_OVF_vect){
//PORTB ^=(1<<PB2);//desligar led

PORTC = step[cont];

TCCR0A ^=(1<<COM0B0) | (1<<COM0B1); //ativar desativar OCRB


//percorrer vetor do step
cont++;
n_lat++;
if(n_lat==4){ //enviar a cada 2 lat
    send();
    n_lat=0;
}
if(cont==8) //zerar vetor step
   cont=0;


  
//TCCR0B ^= 0b00000011; //desativar timer
 
 }//end timer0
 
// ISR(SPI_STC_vect){}


ISR(INT0_vect){
PORTB &=~(1<<PB7);//desligar alimentação motor/resistencia
PORTB |=(1<<PB2);//desligar led

//TCCR0B = 0; // desativar timer
TCCR0B &=~(1<<CS00);
TCCR0B &=~(1<<CS01);


}


ISR(ADC_vect){
   
    if(ADCH<25){ //temperatura maior que 75ºC
     PORTB &=~(1<<PB7);//desligar alimentação motor/resistencia
     PORTB |=(1<<PB2);//desligar led
  //   TCCR0B = 0; //desativa timer
   TCCR0B &=~(1<<CS00);
   TCCR0B &=~(1<<CS01);

    }
}

ISR(USART_RX_vect){
    
    buffer[i] = UDR0;
    
    if(buffer[i] == 13) //carriage feed
    {
       // print();
        tam = i;//memcpy(frase,frase,i);
        i=0;
   
    }
    else{
       i++;
    } 
     

}



int main() {

   


//PORTAS
    
DDRB = 0xff;
DDRC = 0b00001111;
PORTC = 0;

DDRD |=(1<<DDD6);//ocr0a /nao usar
DDRD |= (1<<DDD5);//porta resistencia
DDRD &=~(1<<DDD2); //porta sensor
DDRD &= ~(1<<DDD7);//porta botão
DDRD &=~(1<<DDD4);//teste

PORTD &=~(1<<PD6);//ocr0a /nao usar
PORTD |=(1<<PD2); //pull up sensor
PORTD &=~(1<<PD5);//desativar resitencia
PORTD |= (1<<PD7);//pull up botao
PORTB |=(1<<PB6); //porta LAT
PORTD |=(1<<PD4);//teste

//CONFIGURAR SPI
PORTB |=(1<<PB3) | (1<<PB5) |(1<<PB2);
SPCR = (1<<SPE) | (1<<DORD) |  (1<<MSTR);// | (1<<SPIE);
SPSR = (1<<SPI2X);

//CONFIGURAR UART
UBRR0 = 51;//9600
UCSR0A = 0;
UCSR0B = (1<<RXCIE0) | (1<<RXEN0) | (1<<TXEN0);
UCSR0C = (1<<UCSZ01) | (1<<UCSZ00);
//CONFIGURAR INTERRUPCAO

EICRA = 0;// level baixo (1<<ISC01); //borda de descida gera interrupcao int0
EIMSK = (1<<INT0); //pino 32

//CONFIGURAR ADC

ADMUX = (1<<REFS0) | (1<<MUX0) | (1<<MUX1) | (1<<MUX2) | (1<<ADLAR);
ADCSRA = (1<<ADEN) | (1<<ADSC) | (1<<ADATE) | (1<<ADPS0) | (1<<ADPS1) | (1<<ADPS2)| (1<<ADIE);
ADCSRB = (1<<ADTS2);//Timer/Counter0 overflow


//CONGIFURAR TIMER
TCCR0A = 0;
TCCR0B = 0;

OCR0A = 115;//frequencia ->+
OCR0B = 70;//duty
TCCR0A = (1<<COM0B0) | (1<<COM0B1) | (1<<WGM00) | (1<<WGM01); // setar pino DST pulso, modo invertido setar comparação limpar do topo
TCCR0B = (1<<CS00)| (1<<CS01) | (1<<WGM02); //clock/64

TIMSK0 = (1<<TOIE0);

TCCR0B ^= 0b00000011; //desativar timer
sei();

   
//print();
//PORTC = 0b00001010; //step inicial do motor
   
   
   
            //TCCR0B ^= 0b00000011;
//print();
   
   
  while(1){
        if(!(PIND &(1<<PD7))){
            while(!(PIND &(1<<PD7)));
                     //PORTB ^=(1<<PB2);// desliga led   
            USART_Transmit();
         print();  
         //USART_Transmit();
        }
        /*
        else if(!(PIND &(1<<PD4))){
            while(!(PIND &(1<<PD4)));
                     PORTB ^=(1<<PB2);// desliga led 
        }
        */
        
     };//end while
   return 0;
 }//end main

void string(void){

  
    linhas = (tam/24)+1;    //quantidade de linhas
    
    while(tam != linhas*24){
        buffer[tam]= ' ';    //preencher resto da frase com espaço
        tam++;
    }
    buffer[tam] = 13; //CR no ultimo caracter
    
}

void send(void){

   uint8_t data;
  
  if(buffer[pos+linha] == 13){ //desligar/parar ao chegar ao fim
      TCCR0B &=~(1<<CS00);//desligar timer
      TCCR0B &=~(1<<CS01);
      PORTB &=~(1<<PB7); //desligar power
      PORTB |=(1<<PB2);// desliga led
      pos=0, linha=0, offset=0,li=0,L=0,cont=0;//zerar variaveis, posicao inicial
      memset(buffer,13,500); //limpar buffer

  }
    while(pos <24){
        
// bytes_per_char = font_height * (font_width / 8 + ((font_width % 8) ? 1 : 0))
// offset = (ascii_code(character) - ascii_code(' ')) * bytes_per_char
// data = consolas_20pt[offset]
// 
        
        // bytes_per_char = 32*(16/8+((16%8)? 1:0));
         offset = (buffer[pos+linha] - 32) * 64;
         data = pgm_read_byte_near(&font[offset+li+L]);
         li++;//proximo byte
        // letra = pgm_read_byte_near(&font[(((frase[pos+li]-32)*64)+(byte))]); //enviar uma letra
        SPDR = data;
        while(!(SPSR & (1<<SPIF)));  //aguardar o envio
    
        data = pgm_read_byte_near(&font[offset+li+L]);

        //letra = pgm_read_byte_near(&font[(((frase[pos+li]-32)*64)+(byte+1))]); //enviar uma letra
        SPDR = data;
        while(!(SPSR & (1<<SPIF)));
        
        li=0;//zerar posicao do byte
            ++pos; //proximo caracter
           
        }
  
   pos=0;//posicao na string
   L+=2;//linha de texto
  
  
    PORTB ^=(1<<PB6); // LAT gravar dados
    PORTB ^=(1<<PB6);
    if(L>62){ //proxima linha na impressao
    linha+=24;
    L=0;
    }
    
    
}

void print(void){
    PORTB &=~(1<<PB2); //ligar led
    string();//converter frase da memoria
    send();
    PORTB |=(1<<PB7); // Power
    PORTC = 0b00001010; //step inicial do motor
    TCCR0B ^= 0b00000011; //ativar timer
    
}

void USART_Transmit(void)
{
/* Wait for empty transmit buffer */
    while(buffer[i] != 13){
        PORTB &=~(1<<PB2); //ligar led
while (!(UCSR0A & (1<<UDRE0)));
/* Put data into buffer, sends the data */
UDR0 = buffer[i];
i++;
    }
    i=0;
    PORTB |=(1<<PB2); //desligar led
}
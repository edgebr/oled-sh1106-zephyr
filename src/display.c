/**
 * @file display.c
 * @author Paulo Santos (pauloroberto.santos@edge.ufal.br)
 * @brief Implementa os drivers do display.
 * @version 0.1
 * @date 15-06-2023
 *
 * @copyright Copyright (c) 2023, Centro de Inovação EDGE.
 *
 */

#include "display.h"

#include <string.h>

#include <zephyr/drivers/gpio.h>
#if CONFIG_DISPLAY_SPI == 1
#include <zephyr/drivers/spi.h>
#endif

/**
 * @brief Altura/largura em pixels de um byte.
 */
#define BYTE_BITS 8

/**
 * @brief Número de bytes nos buffers de tela do display.
 */
#define SCREEN_BUFFER_BYTES ((SCR_W * SCR_H) / 8)

/**
 * @brief Timeout do mutex para a escrita de conteúdo na VRAM do display.
 */
#define DISPLAY_MUTEX_LOCK_TIMEOUT_MS 100

/**
 * @brief Obtém o resultado de um bitwise and entre o campo e a máscara desejados.
 */
#define CHECK_MASK(target_field, mask) ((target_field) & (mask))

/**
 * @brief Atribui os valores presentes na máscara, bit a bit, no campo desejado.
 */
#define SET_MASK(target_field, mask) (target_field) |= (mask)

/**
 * @brief Limpa os valores presentes na máscara, bit a bit, do campo desejado.
 */
#define CLEAR_MASK(target_field, mask) (target_field) &= ~(mask)

/**
 * @brief Inverte os valores de um campo presentes na máscara, bit a bit.
 */
#define INVERT_MASK(target_field, mask) (target_field) ^= (mask)

/**
 * @brief Cria o mutex de controle da VRAM, para evitar que mais de um módulo acesse a região ao
 * mesmo tempo.
 *
 * @note A VRAM representa o conteúdo enviado ao display físico.
 */
K_MUTEX_DEFINE(vram_mutex);

#if CONFIG_DISPLAY_SPI == 1
/**
 * @brief Configuração do SPI do display (Modo 3-wire, sem GPIO Data/Command).
 */
#define SPI_FLAGS (SPI_OP_MODE_MASTER | SPI_WORD_SET(9) | SPI_LINES_SINGLE)

/**
 * @brief Estrutura que representa o SPI do display.
 */
static const struct spi_dt_spec display_spi =
	SPI_DT_SPEC_GET(DT_NODELABEL(display_spi), SPI_FLAGS, 0);

/**
 * @brief Buffer de comandos, com tamanho máximo de 2 comandos sequenciais.
 */
static struct spi_buf cmds[1];

/**
 * @brief Conjunto de buffers para a SPI.
 */
static struct spi_buf_set tx_data = {
	.buffers = NULL,
	.count = 1,
};

/**
 * @brief Buffer de dados de uma página para RAM do display..
 */
static struct spi_buf page_data = {
	.buf = NULL,
	.len = SCR_W,
};

/**
 * @brief Configura a comunicação SPI com o display para envio de comandos.
 */
static inline void set_command(void) { tx_data.buffers = cmds; }

/**
 * @brief Configura a comunicação SPI com o display para envio de dados para RAM.
 */
static inline void set_data(void) { tx_data.buffers = &page_data; }

/**
 * @brief Envia um comando para o display.
 *
 * @param cmd Byte a ser enviado.
 */
static void display_send_cmd(enum display_commands cmd);

/**
 * @brief Envia um comando de dois bytes para o display.
 *
 * @param cmd Byte de comando.
 * @param follow_up Segundo byte.
 */
static void display_send_double_cmd(enum display_commands cmd,
                                    uint8_t follow_up);

/**
 * @brief Envia uma sequência de bytes como dados para RAM do display.
 */
static void display_send_page(void);
#endif /* #elif CONFIG_DISPLAY_VIRTUAL == 1 */

/**
 * @brief Desenha um pixel na tela.
 *
 * @param x Posição horizontal do pixel.
 * @param y Posição vertical do pixel.
 */
static void draw_pixel(uint8_t x, uint8_t y);

/**
 * @brief Desenha uma linha horizontal no display, na orientação padrão.
 *
 * @param x Posição horizontal do início da linha.
 * @param y Posição vertical do início da linha.
 * @param w Tamanho da linha.
 */
static void draw_horizontal_line_base(uint8_t x, uint8_t y, uint8_t w);

/**
 * @brief Desenha uma linha vertical no display, na orientação padrão.
 *
 * @param x Posição horizontal do início da linha.
 * @param y Posição vertical do início da linha.
 * @param h Tamanho da linha.
 */
static void draw_vertical_line_base(uint8_t x, uint8_t y, uint8_t h);

/**
 * @brief Desenha uma linha horizontal na tela.
 *
 * @param origin_x Posição horizontal na extremidade esquerda da linha.
 * @param end_x Posição horizontal na extremidade direita da linha.
 * @param y Posição vertical da linha.
 */
static void display_draw_h_line(uint8_t origin_x, uint8_t end_x, uint8_t y);

/**
 * @brief Desenha uma linha vertical na tela.
 *
 * @param origin_y Posição vertical da extremidade superior da linha.
 * @param end_y Posição vertical da extremidade inferior da linha.
 * @param x Posição horizontal da linha.
 */
static void display_draw_v_line(uint8_t origin_y, uint8_t end_y, uint8_t x);

/**
 * @brief Imprime um caractere na tela.
 *
 * @param x Posição horizontal esquerda do caractere.
 * @param y Posição vertical superior do caractere.
 * @param character Caractere a ser desenhando.
 * @param[in] fnt Fonte a ser utilizada.
 * @param scale Escala de impressão.
 * @return Posição horizontal direita do caractere após o desenho.
 */
static uint8_t print_char(uint8_t x, uint8_t y, char character,
                          const struct font *fnt, uint8_t scale);

/**
 * @brief Desenha o padrão de um byte no display, verticalmente, sem considerar
 * a orientação do display.
 * @param top Posição vertical onde o byte deve ser desenhado.
 * @param left Posição horizontal onde o byte deve ser desenhado.
 * @param byte Byte a ser desenhado.
 * @param scale Escala de impressão.
 */
static inline void draw_byte_v(uint8_t top, uint8_t left, uint8_t byte,
                               uint8_t scale);

/**
 * @brief Desenha o padrão de um byte no display, horizontalmente, sem
 * considerar a orientação do display.
 * @param top Posição vertical onde o byte deve ser desenhado.
 * @param left Posição horizontal onde o byte deve ser desenhado.
 * @param byte Byte a ser desenhado.
 * @param scale Escala de impressão.
 */
static inline void draw_byte_h(uint8_t top, uint8_t left, uint8_t byte,
                               uint8_t scale);

/**
 * @brief Imprime um bitmap com leitura vertical.
 *
 * @param[in] bmp Bitmap a ser impresso.
 * @param top Posição do canto superior do bitmap.
 * @param left Posição do canto esquerdo do bitmap.
 * @param width Largura do bitmap.
 * @param height Altura do bitmap.
 * @param scale Escala de impressão.
 */
static inline void print_bmp_vertical(const uint8_t *bmp, uint8_t top,
                                      uint8_t left, uint8_t width,
                                      uint8_t height, uint8_t scale);

/**
 * @brief Imprime um bitmap com leitura horizontal.
 *
 * @param[in] bmp Bitmap a ser impresso.
 * @param top Posição do canto superior do bitmap.
 * @param left Posição do canto esquerdo do bitmap.
 * @param width Largura do bitmap.
 * @param height Altura do bitmap.
 * @param scale Escala de impressão.
 */
static inline void print_bmp_horizontal(const uint8_t *bmp, uint8_t top,
                                        uint8_t left, uint8_t width,
                                        uint8_t height, uint8_t scale);

/**
 * @brief Estrutura os dados de controle do display.
 */
static struct display_control {
#if CONFIG_DISPLAY_VIRTUAL == 1
  const uint8_t marker[sizeof(
      CONFIG_VIRTUAL_DISPLAY_MARKER_STR)]; /**< Marcador de início do buffer de
                                              vídeo. */
  uint8_t
      screen_buffer[SCREEN_BUFFER_BYTES]; /**< Segundo buffer de vídeo, 128x64
                                           * pixels, armazenados em bytes.*/
#endif                                    /* #if CONFIG_DISPLAY_VIRTUAL == 1 */
  uint8_t draw_buffer[SCREEN_BUFFER_BYTES]; /**< Buffer de vídeo, 128x64 pixels,
                                             * armazenados em bytes.*/
  enum display_draw_mode pixel_mode;        /**< Modo atual de desenho.*/
} self = {
#if CONFIG_DISPLAY_VIRTUAL == 1
    .marker = CONFIG_VIRTUAL_DISPLAY_MARKER_STR,
    .screen_buffer = {},
#endif /* #if CONFIG_DISPLAY_VIRTUAL == 1 */
    .pixel_mode = DISPLAY_PIXEL_WHITE,
    .draw_buffer = {},
};

void display_init(void) {
#if CONFIG_DISPLAY_VIRTUAL == 1
  /* Envia via RTT a posição da vRAM do display para o emulador. */
  printk("D-VRAM: %p\n", self.screen_buffer);

  memset(self.screen_buffer, 0x00, sizeof(self.screen_buffer));
#elif CONFIG_DISPLAY_SPI == 1 /* #if CONFIG_DISPLAY_VIRTUAL == 1 */
  gpio_pin_configure_dt(&data_command, GPIO_OUTPUT_INACTIVE);

  while (!spi_is_ready_dt(&display_spi)) {
    /** Espera a comunicação SPI estar pronta. */
  }

  display_send_cmd(DISPLAY_CMD_DISP_OFF);

  /* Clock máximo. */
  display_send_double_cmd(DISPLAY_CMD_CLOCKDIV, 0xF0);

  /* 64 linhas. */
  display_send_double_cmd(DISPLAY_CMD_SET_MUX, 63);

  /* Offset 0. */
  display_send_double_cmd(DISPLAY_CMD_SET_OFFSET, 0x00);

  display_send_cmd(DISPLAY_CMD_START_LINE);

  /* Liga o DC-DC interno para o display: true = ligado; false = desligado. */
  display_send_double_cmd(DISPLAY_CMD_DC_DC_SET, 0x8A | false);

  /* Configura a rotação da tela para retrato. */
  display_send_cmd(DISPLAY_CMD_SEG_INV);
  display_send_cmd(DISPLAY_CMD_COM_INV);

  /* Configuração de hardware alternativa. */
  display_send_double_cmd(DISPLAY_CMD_COM_HW, 0x12);

  /* Contraste padrão. */
  display_send_double_cmd(DISPLAY_CMD_CONTRAST, 0x80);

  /* 2 ciclos de clock para descarga e 10 ciclos para pré-carga.*/
  display_send_double_cmd(DISPLAY_CMD_SET_CHARGE, 0xFF);

  /* Configura VCOM igual a VREF para pré-carga mais rápida. */
  display_send_double_cmd(DISPLAY_CMD_VCOM_DSEL, 0x20);

  display_send_cmd(DISPLAY_DC_DC_VOLTAGE);

  display_send_cmd(DISPLAY_CMD_INV_OFF);

  display_send_cmd(DISPLAY_CMD_DISPLAY_BY_RAM);

  display_send_cmd(DISPLAY_CMD_PAGE_ADDR);

  /* Configura a coluna inicial de escrita. */
  display_send_double_cmd(DISPLAY_CMD_COL_LOW, DISPLAY_CMD_COL_HIGH);

  display_flush();

  display_send_cmd(DISPLAY_CMD_DISP_ON);
  k_msleep(100);
#endif                        /* #elif CONFIG_DISPLAY_SPI == 1 */
}

void display_disable(void) {
#if CONFIG_DISPLAY_SPI
  display_send_cmd(DISPLAY_CMD_DISP_OFF);
#endif
}

void display_enable(void) {
#if CONFIG_DISPLAY_SPI
  display_send_cmd(DISPLAY_CMD_DISP_ON);
#endif
}

void display_clear(void) {
  if (k_mutex_lock(&vram_mutex, K_MSEC(DISPLAY_MUTEX_LOCK_TIMEOUT_MS)) != 0) {
    return;
  }

  memset(self.draw_buffer, 0x00, sizeof(self.draw_buffer));

  k_mutex_unlock(&vram_mutex);
}

void display_flush(void) {
#if CONFIG_DISPLAY_VIRTUAL == 1
  memcpy(self.screen_buffer, self.draw_buffer, sizeof(self.screen_buffer));
#else  /* #if CONFIG_DISPLAY_VIRTUAL == 1 */
  if (k_mutex_lock(&vram_mutex, K_MSEC(DISPLAY_MUTEX_LOCK_TIMEOUT_MS)) != 0) {
    return;
  }

  display_send_double_cmd(DISPLAY_CMD_COL_LOW, DISPLAY_CMD_COL_HIGH);

  for (uint8_t page = 0; page < 8; page++) {
    display_send_cmd(DISPLAY_CMD_PAGE_ADDR + page);
    display_send_cmd(DISPLAY_CMD_RMW_START);

    uintptr_t current_page = self.draw_buffer + (page << 7);

    uint16_t three_wire_data_buf[page_data.len] = {0};
    for (size_t i = 0; i < page_data.len; i++) {
      three_wire_data_buf[i] = BIT(9) | current_page[i];
    }
    page_data.buf = three_wire_data_buf;

    display_send_page();

    display_send_cmd(DISPLAY_CMD_RMW_END);
    k_msleep(2);
  }
  display_send_cmd(DISPLAY_CMD_SEG_INV);
  display_send_cmd(DISPLAY_CMD_COM_INV);
  display_send_cmd(DISPLAY_CMD_START_LINE);

  k_mutex_unlock(&vram_mutex);
#endif /* #if CONFIG_DISPLAY_VIRTUAL == 1 #else */
}

void display_draw_line(uint8_t origin_x, uint8_t origin_y, const uint8_t end_x,
                       const uint8_t end_y) {
  int16_t dx = (int16_t)(end_x - origin_x);
  int16_t dy = (int16_t)(end_y - origin_y);
  int16_t di;
  const int8_t dx_sym = (dx > 0) ? 1 : -1;
  const int8_t dy_sym = (dy > 0) ? 1 : -1;

  if (dx == 0) {
    display_draw_v_line(origin_y, end_y, origin_x);
    return;
  }
  if (dy == 0) {
    display_draw_h_line(origin_x, end_x, origin_y);
    return;
  }

  /* Algoritmo de desenho de linha por diferencial. */
  dx = (int16_t)(dx * dx_sym);
  dy = (int16_t)(dy * dy_sym);
  const int16_t dx2 = (int16_t)(dx << 1);
  const int16_t dy2 = (int16_t)(dy << 1);

  if (dx >= dy) {
    di = (int16_t)(dy2 - dx);
    while (origin_x != end_x) {
      draw_pixel(origin_x, origin_y);
      origin_x += dx_sym;
      if (di < 0) {
        di = (int16_t)(di + dy2);
      } else {
        di = (int16_t)(di + (dy2 - dx2));
        origin_y += dy_sym;
      }
    }
  } else {
    di = (int16_t)(dx2 - dy);
    while (origin_y != end_y) {
      draw_pixel(origin_x, origin_y);
      origin_y += dy_sym;
      if (di < 0) {
        di = (int16_t)(di + dx2);
      } else {
        di = (int16_t)(di + (dx2 - dy2));
        origin_x += dx_sym;
      }
    }
  }

  draw_pixel(origin_x, origin_y);
}

void display_draw_rect(uint8_t top, uint8_t left, const uint8_t height,
                       const uint8_t width, const bool filled) {
  uint8_t right = left + width;
  uint8_t bottom = top + height;
  display_draw_h_line(left, right, top);
  display_draw_h_line(left, right, bottom);
  display_draw_v_line(top + 1, bottom, left);
  display_draw_v_line(top, bottom - 1, right);

  if (filled) {
    top += 1;
    left += 1;
    bottom -= 1;
    while (right > left) {
      right -= 1;
      display_draw_v_line(top, bottom, right);
    }
  }
}

void display_draw_circle(const uint8_t center_x, const uint8_t center_y,
                         const uint8_t radius) {
  int16_t err = (int16_t)(1 - radius);
  int16_t dx = 0;
  int16_t dy = (int16_t)(-2 * radius);
  int16_t x = 0;
  int16_t y = radius;

  const int16_t height_limit = SCR_H - 1;
  const int16_t width_limit = SCR_W - 1;

  while (x < y) {
    if (err >= 0) {
      dy += 2;
      err = (int16_t)(err + dy);
      y -= 1;
    }

    dx += 2;
    err = (int16_t)(err + dx + 1);
    x += 1;

    /* Desenho dos octantes. */
    if (center_x + x < width_limit) {
      if (center_y + y < height_limit) {
        draw_pixel((uint8_t)(center_x + x), (uint8_t)(center_y + y));
      }
      if (center_y - y > -1) {
        draw_pixel((uint8_t)(center_x + x), (uint8_t)(center_y - y));
      }
    }
    if (center_x - x > -1) {
      if (center_y + y < height_limit) {
        draw_pixel((uint8_t)(center_x - x), (uint8_t)(center_y + y));
      }
      if (center_y - y > -1) {
        draw_pixel((uint8_t)(center_x - x), (uint8_t)(center_y - y));
      }
    }
    if (center_x + y < width_limit) {
      if (center_y + x < height_limit) {
        draw_pixel((uint8_t)(center_x + y), (uint8_t)(center_y + x));
      }
      if (center_y - x > -1) {
        draw_pixel((uint8_t)(center_x + y), (uint8_t)(center_y - x));
      }
    }
    if (center_x - y > -1) {
      if (center_y + x < height_limit) {
        draw_pixel((uint8_t)(center_x - y), (uint8_t)(center_y + x));
      }
      if (center_y - x > -1) {
        draw_pixel((uint8_t)(center_x - y), (uint8_t)(center_y - x));
      }
    }
  }

  /* Pontos verticais e horizontais. */
  if (center_x + radius < width_limit) {
    draw_pixel(center_x + radius, center_y);
  }
  if (center_x - radius > -1) {
    draw_pixel(center_x - radius, center_y);
  }
  if (center_y + radius < height_limit) {
    draw_pixel(center_x, center_y + radius);
  }
  if (center_y - radius > -1) {
    draw_pixel(center_x, center_y - radius);
  }
}

void display_draw_bitmap(const uint8_t *bmp, const uint8_t top,
                         const uint8_t left, const uint8_t width,
                         const uint8_t height, const uint8_t scale) {
  if (bmp == NULL) {
    return;
  }

  print_bmp_horizontal(bmp, top, left, width, height, scale);
}

uint16_t display_print(const uint8_t *str, uint8_t top, const uint8_t left,
                       const enum font_sizes font, const uint8_t scale) {
  if ((str == NULL) || (scale == 0)) {
    return 0;
  }

  uint8_t p_x = left;
  const struct font *font_p = fonts_get(font);
  const uint8_t width_limit = (uint8_t)(SCR_W - (font_p->width * scale) - 1);

  while ((*str != '\0') && (p_x < width_limit)) {
    p_x += print_char(p_x, top, *str, font_p, scale) * scale;
    str += 1;
    if (*str == '\n') {
      top += font_p->height + 1;
      p_x = left;
      str += 1;
    }
  }

  return p_x - left;
}

void display_set_draw_mode(const enum display_draw_mode mode) {
  self.pixel_mode = mode;
}

#if CONFIG_DISPLAY_VIRTUAL != 1
void display_set_contrast(uint8_t amount) {
  display_send_double_cmd(DISPLAY_CMD_CONTRAST, amount);
}

static void display_send_cmd(enum display_commands cmd) {
  uint16_t three_wire_buf[] = {cmd};
  cmds[0].buf = buf;
  cmds[0].len = 1;

  set_command();
  spi_write_dt(&display_spi, &tx_data);
}

static void display_send_double_cmd(enum display_commands cmd,
                                    uint8_t follow_up) {
  uint16_t three_wire_buf[] = {cmd, follow_up};
  cmds[0].buf = buf;
  cmds[0].len = 2;

  set_command();
  spi_write_dt(&display_spi, &tx_data);
}

static void display_send_page(void) {
  set_data();
  spi_write_dt(&display_spi, &tx_data);
}
#endif /* #if CONFIG_DISPLAY_VIRTUAL != 1 */

static void draw_pixel(const uint8_t x, const uint8_t y) {

  const register uint32_t vram_offset = ((y >> 3) << 7) + x;
  const register uint32_t bit_pos = y & 0x07;

  if (vram_offset >= ((SCR_W * SCR_H) >> 3)) {
    return;
  }

  if (k_mutex_lock(&vram_mutex, K_MSEC(DISPLAY_MUTEX_LOCK_TIMEOUT_MS)) != 0) {
    return;
  }

  switch (self.pixel_mode) {
  case DISPLAY_PIXEL_WHITE:
    SET_MASK(self.draw_buffer[vram_offset], 1 << bit_pos);
    break;
  case DISPLAY_PIXEL_BLACK:
    CLEAR_MASK(self.draw_buffer[vram_offset], 1 << bit_pos);
    break;
  case DISPLAY_PIXEL_INVERT:
    INVERT_MASK(self.draw_buffer[vram_offset], 1 << bit_pos);
    break;
  }
  k_mutex_unlock(&vram_mutex);
}

static void draw_horizontal_line_base(const uint8_t x, const uint8_t y,
                                      uint8_t w) {

  const uint8_t mask = (uint8_t)(1 << (y & 0x07));
  uint8_t *ptr = &self.draw_buffer[(y >> 3) << 7] + x;

  if (k_mutex_lock(&vram_mutex, K_MSEC(DISPLAY_MUTEX_LOCK_TIMEOUT_MS)) != 0) {
    return;
  }

  switch (self.pixel_mode) {
  case DISPLAY_PIXEL_WHITE:
    for (; w != 0; w--) {
      SET_MASK(*ptr, mask);
      ptr += 1;
    }
    break;
  case DISPLAY_PIXEL_BLACK:
    for (; w != 0; w--) {
      CLEAR_MASK(*ptr, mask);
      ptr += 1;
    }
    break;
  case DISPLAY_PIXEL_INVERT:
    for (; w != 0; w--) {
      INVERT_MASK(*ptr, mask);
      ptr += 1;
    }
    break;
  }

  k_mutex_unlock(&vram_mutex);
}

static void draw_vertical_line_base(const uint8_t x, const uint8_t y,
                                    uint8_t h) {
  uint8_t mask;
  uint8_t mod;
  uint8_t *ptr = &self.draw_buffer[(y >> 3) << 7] + x;

  /* Tabela de máscaras para o primeiro byte. */
  static const uint8_t first_byte_table[] = {
      0x00, 0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE,
  };

  /* Tabela de máscaras para o último byte. */
  static const uint8_t last_byte_table[] = {
      0x00, 0x01, 0x03, 0x07, 0x0F, 0x1F, 0x3F, 0x7F,
  };

  if (k_mutex_lock(&vram_mutex, K_MSEC(DISPLAY_MUTEX_LOCK_TIMEOUT_MS)) != 0) {
    return;
  }

  /* Verifica o primeiro byte. */
  if (y & 0x07) {
    mod = 8 - (y & 0x07);
    mask = first_byte_table[mod];

    /* Verifica se a linha precisa da máscara completa. */
    if (mod > h) {
      mask &= 0xFF >> (mod - h);
    }

    switch (self.pixel_mode) {
    case DISPLAY_PIXEL_WHITE:
      SET_MASK(*ptr, mask);
      break;
    case DISPLAY_PIXEL_BLACK:
      CLEAR_MASK(*ptr, mask);
      break;
    case DISPLAY_PIXEL_INVERT:
      INVERT_MASK(*ptr, mask);
      break;
    }

    if (mod > h) {
      return;
    }

    ptr += SCR_W;
    h -= mod;
  }

  /* Bytes intermediários. */
  switch (self.pixel_mode) {
  case DISPLAY_PIXEL_WHITE:
    for (; h > 7; h -= 8) {
      *ptr = 0xFF;
      ptr += SCR_W;
    }
    break;
  case DISPLAY_PIXEL_BLACK:
    for (; h > 7; h -= 8) {
      *ptr = 0x00;
      ptr += SCR_W;
    }
    break;
  case DISPLAY_PIXEL_INVERT:
    for (; h > 7; h -= 8) {
      *ptr = ~*ptr;
      ptr += SCR_W;
    }
    break;
  }

  /* Verifica o ultimo byte. */
  if (h) {
    mod = h & 0x07;
    mask = last_byte_table[mod];

    switch (self.pixel_mode) {
    case DISPLAY_PIXEL_WHITE:
      SET_MASK(*ptr, mask);
      break;
    case DISPLAY_PIXEL_BLACK:
      CLEAR_MASK(*ptr, mask);
      break;
    case DISPLAY_PIXEL_INVERT:
      INVERT_MASK(*ptr, mask);
      break;
    }
  }
  k_mutex_unlock(&vram_mutex);
}

static uint8_t print_char(const uint8_t x, const uint8_t y, char character,
                          const struct font *fnt, const uint8_t scale) {
  const uint8_t *char_bmp;

  if (character < fnt->min_char || character > fnt->max_char) {
    character = fnt->max_char;
  }

  if (fnt->is_scan_vertical) {
    char_bmp = &fnt->characters[(character - fnt->min_char) * fnt->width];
    print_bmp_vertical(char_bmp, y, x, fnt->width, fnt->height, scale);
  } else {
    char_bmp = &fnt->characters[(character - fnt->min_char) * fnt->height];
    print_bmp_horizontal(char_bmp, y, x, fnt->width, fnt->height, scale);
  }

  return fnt->width + 1;
}

static inline void draw_byte_v(uint8_t top, const uint8_t left, uint8_t byte,
                               const uint8_t scale) {
  for (; byte != 0; byte >>= 1) {
    if (byte & 0x01) {
      for (uint8_t i = 0; i < scale; i++) {
        for (uint8_t j = 0; j < scale; j++) {
          draw_pixel(left + i, top + j);
        }
      }
    }

    top += scale;
  }
}

static inline void draw_byte_h(const uint8_t top, uint8_t left, uint8_t byte,
                               const uint8_t scale) {
  for (; byte != 0; byte <<= 1) {
    if (byte & 0x80) {
      for (uint8_t i = 0; i < scale; i++) {
        for (uint8_t j = 0; j < scale; j++) {
          draw_pixel(left + i, top + j);
        }
      }
    }

    left += scale;
  }
}

static inline void print_bmp_vertical(const uint8_t *bmp, uint8_t top,
                                      const uint8_t left, const uint8_t width,
                                      const uint8_t height,
                                      const uint8_t scale) {
  for (uint8_t i = 0; i < height; i += BYTE_BITS) {
    uint8_t x_pos = left;
    for (uint8_t j = 0; j < width; j++) {
      if (*bmp != 0) {
        draw_byte_v(top, x_pos, *bmp, scale);
      }
      bmp += 1;
      x_pos += scale;
    }
    top += BYTE_BITS * scale;
  }
}

static inline void print_bmp_horizontal(const uint8_t *bmp, uint8_t top,
                                        const uint8_t left, const uint8_t width,
                                        const uint8_t height,
                                        const uint8_t scale) {

  for (uint8_t i = 0; i < height; i++) {
    uint8_t x_pos = left;
    for (uint8_t j = 0; j < width; j += BYTE_BITS) {
      if (*bmp != 0) {
        draw_byte_h(top, x_pos, *bmp, scale);
      }
      bmp += 1;
      x_pos += BYTE_BITS * scale;
    }
    top += scale;
  }
}

static void display_draw_h_line(const uint8_t origin_x, const uint8_t end_x,
                                const uint8_t y) {
  const uint8_t w = end_x - origin_x + 1;

  draw_horizontal_line_base(origin_x, y, w);
}

static void display_draw_v_line(const uint8_t origin_y, const uint8_t end_y,
                                const uint8_t x) {
  const uint8_t h = end_y - origin_y + 1;

  draw_vertical_line_base(x, origin_y, h);
}

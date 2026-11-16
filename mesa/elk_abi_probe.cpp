#include <cstring>
#include "intel/compiler/elk/elk_reg.h"

extern "C" bool
hasvk_elk_cpp_abi_check(const struct elk_reg *input, struct elk_reg *output, unsigned c_size)
{
   if (c_size != sizeof(struct elk_reg) || sizeof(struct elk_reg) != 16 ||
       input->file != VGRF || input->type != ELK_REGISTER_TYPE_UW ||
       input->subnr != 2 || input->nr != 17 || !input->negate || !input->abs)
      return false;
   const unsigned expected = unsigned(ELK_REGISTER_TYPE_UW) | (unsigned(VGRF) << 4) |
                             (1u << 7) | (1u << 8) | (2u << 27);
   if (input->bits != expected) return false;
   memset(output, 0, sizeof(*output));
   output->file = BAD_FILE;
   output->type = ELK_REGISTER_TYPE_UD;
   output->subnr = 31; output->nr = 99; output->writemask = 15;
   return output->file == BAD_FILE && output->type == ELK_REGISTER_TYPE_UD;
}

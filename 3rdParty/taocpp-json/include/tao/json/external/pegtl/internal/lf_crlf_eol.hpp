// Copyright (c) 2016-2020 Dr. Colin Hirsch and Daniel Frey
// Please see LICENSE for license or visit https://github.com/taocpp/PEGTL/

#ifndef TAO_JSON_PEGTL_INTERNAL_LF_CRLF_EOL_HPP
#define TAO_JSON_PEGTL_INTERNAL_LF_CRLF_EOL_HPP

#include "../config.hpp"
#include "../eol_pair.hpp"

namespace TAO_JSON_PEGTL_NAMESPACE::internal
{
   struct lf_crlf_eol
   {
      static constexpr int ch = '\n';

      template< typename Input >
      [[nodiscard]] static eol_pair match( Input& in ) noexcept( noexcept( in.size( 2 ) ) )
      {
         eol_pair p = { false, in.size( 2 ) };
         if( p.second ) {
            const auto a = in.peek_char();
            if( a == '\n' ) {
               in.bump_to_next_line();
               p.first = true;
            }
            else if( ( a == '\r' ) && ( p.second > 1 ) && ( in.peek_char( 1 ) == '\n' ) ) {
               in.bump_to_next_line( 2 );
               p.first = true;
            }
         }
         return p;
      }
   };

}  // namespace TAO_JSON_PEGTL_NAMESPACE::internal

#endif

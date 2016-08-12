/ / / / 
 # i n c l u d e   < s t d i o . h > 
 # i n c l u d e   < s t d l i b . h > 
 # i n c l u d e   < s t d i n t . h > 
 # i n c l u d e   < s t d a r g . h > 
 
 # i n c l u d e   < W i n d o w s . h > 
 # i n c l u d e   < i o . h > 
 
 c l a s s   W C h a r a c t e r s   { 
 p r i v a t e : 
     w c h a r _ t   * w s t r ; 
     u i n t 3 2 _ t   l e n ; 
 
 p u b l i c : 
     W C h a r a c t e r s ( c o n s t   c h a r   * s t r ,   s i z e _ t   s i z e )   :   w s t r ( n u l l p t r )   { 
         i f   ( s t r   = =   n u l l p t r ) 
             r e t u r n ; 
         i n t   u n i c o d e L e n   =   : : M u l t i B y t e T o W i d e C h a r ( C P _ U T F 8 ,   0 ,   s t r ,   s i z e ,   N U L L ,   0 ) ; 
         i f   ( u n i c o d e L e n   = =   0 ) 
             r e t u r n ; 
         w s t r   =   n e w   w c h a r _ t [ u n i c o d e L e n   +   1 ] ; 
         i f   ( w s t r   = =   n u l l p t r ) 
             r e t u r n ; 
         w s t r [ u n i c o d e L e n ]   =   0 ; 
         : : M u l t i B y t e T o W i d e C h a r ( C P _ U T F 8 ,   0 ,   s t r ,   - 1 ,   ( L P W S T R ) w s t r ,   u n i c o d e L e n ) ; 
         l e n   =   u n i c o d e L e n ; 
     } 
     c o n s t   w c h a r _ t   * G e t ( )   { 
         i f   ( ! w s t r ) 
             r e t u r n   n u l l p t r ; 
         r e t u r n   c o n s t _ c a s t < c o n s t   w c h a r _ t   * > ( w s t r ) ; 
     } 
     u i n t 3 2 _ t   L e n g t h ( )   c o n s t   {   r e t u r n   l e n ;   } 
     ~ W C h a r a c t e r s ( )   { 
         i f   ( w s t r ) 
             d e l e t e [ ]   w s t r ; 
     } 
 } ; 
 
 b o o l   I s U n d e r C o n h o s t ( )   { 
     H A N D L E   h S t d e r r   =   r e i n t e r p r e t _ c a s t < H A N D L E > ( _ g e t _ o s f h a n d l e ( _ f i l e n o ( s t d e r r ) ) ) ; 
     r e t u r n   G e t F i l e T y p e ( h S t d e r r )   = =   F I L E _ T Y P E _ C H A R ; 
 } 
 
 / /   b o o l   I s U n d e r C o n h o s t ( )   { 
 / /       r e t u r n   _ i s a t t y ( _ f i l e n o ( s t d e r r ) )   ! =   0 ; 
 / /   } 
 
 i n t   B a s e E r r o r W r i t e C o n h o s t ( c o n s t   c h a r   * b u f ,   s i z e _ t   l e n )   { 
     / / 
     H A N D L E   h C o n s o l e   =   G e t S t d H a n d l e ( S T D _ E R R O R _ H A N D L E ) ; 
     C O N S O L E _ S C R E E N _ B U F F E R _ I N F O   c s b i ; 
     G e t C o n s o l e S c r e e n B u f f e r I n f o ( h C o n s o l e ,   & c s b i ) ; 
     W O R D   o l d C o l o r   =   c s b i . w A t t r i b u t e s ; 
     W O R D   n e w C o l o r   =   ( o l d C o l o r   &   0 x F 0 )   |   F O R E G R O U N D _ I N T E N S I T Y   |   F O R E G R O U N D _ R E D ; 
     S e t C o n s o l e T e x t A t t r i b u t e ( h C o n s o l e ,   n e w C o l o r ) ; 
     D W O R D   d w W r i t e ; 
     W C h a r a c t e r s   w s t r ( b u f ,   l e n ) ; 
     W r i t e C o n s o l e W ( h C o n s o l e ,   w s t r . G e t ( ) ,   w s t r . L e n g t h ( ) ,   & d w W r i t e ,   n u l l p t r ) ; 
     S e t C o n s o l e T e x t A t t r i b u t e ( h C o n s o l e ,   o l d C o l o r ) ; 
     r e t u r n   0 ; 
 } 
 # i n c l u d e   < S a l . h > 
 i n t   B a s e E r r o r M e s s a g e P r i n t ( _ P r i n t f _ f o r m a t _ s t r i n g _   c o n s t   c h a r   * f o r m a t ,   . . . ) ; 
 i n t   B a s e E r r o r M e s s a g e P r i n t ( c o n s t   c h a r   * f o r m a t ,   . . . )   { 
     s t a t i c   b o o l   c o n h o s t _   =   I s U n d e r C o n h o s t ( ) ; 
     c h a r   b u f [ 1 6 3 4 8 ] ; 
     v a _ l i s t   a p ; 
     v a _ s t a r t ( a p ,   f o r m a t ) ; 
     a u t o   l   =   v s n p r i n t f ( b u f ,   1 6 3 4 8 ,   f o r m a t ,   a p ) ; 
     v a _ e n d ( a p ) ; 
     i f   ( c o n h o s t _ )   { 
         r e t u r n   B a s e E r r o r W r i t e C o n h o s t ( b u f ,   l ) ; 
     } 
     r e t u r n   f w r i t e ( b u f ,   1 ,   l ,   s t d e r r ) ; 
 } 
 
 i n t   m a i n ( )   { 
     / / / / 
     i f   ( I s U n d e r C o n h o s t ( ) )   { 
         M e s s a g e B o x W ( n u l l p t r ,   L " R u n   U n d e r   C o n s o l e H o s t " ,   L " T i t l e " ,   M B _ O K ) ; 
     }   e l s e   { 
         M e s s a g e B o x W ( n u l l p t r ,   L " N o t   C o n s o l e H o s t " ,   L " T i t l e " ,   M B _ O K ) ; 
     } 
     c h a r   e r r [ ]   =   u 8 " ُ/f N*N��" ; 
     B a s e E r r o r M e s s a g e P r i n t ( " C h e c k   e r r o r ,   b l o c k :   % s \ n " ,   e r r ) ; 
     r e t u r n   0 ; 
 } 
 
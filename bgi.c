
/* initwindow example */ 

#include "graphics.h"



int main(void)

{

       /* initialize graphics window at 400 x 300 */

          initwindow(400, 300);



             /* draw a line */

                line(0, 0, getmaxx(), getmaxy());



                   /* clean up */

                      getch();

                         closegraph();

                            return 0;

}


/* initwindow example with two windows */ 

#include "graphics.h"



int main(void)

{

       int wid1, wid2;



          /* initialize graphics windows */

             wid1 = initwindow(400, 300);

                wid2 = initwindow(300, 400, 200, 100);



                   /* draw lines */

                      setcurrentwindow(wid1);

                         line(0, 0, getmaxx(), getmaxy());

                            setcurrentwindow(wid2);

                               line(0, 0, getmaxx(), getmaxy());



                                  /* clean up */

                                     getch();

                                        closegraph();

                                           return 0;

}

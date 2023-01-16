#include <stdio.h> 
struct LinkedList{ 
    int data; 
    struct LinkedList *next; 
}; 
typedef struct LinkedList *node; 
node createNode(){ 
    node temp; // declare a node 
    temp = (node)malloc(sizeof(struct LinkedList)); // allocate memory using malloc() 
}
node addNode(node head, int value){ 
    node temp,p;// declare two nodes temp and p 
    temp = createNode();//createNode will return a new node with data = value and next pointing to NULL. 
    temp->data = value; // add element's value to data part of node 
    if(head == NULL){ 
        head = temp;     //when linked list is empty 
    } 
    else{ 
        p  = head;//assign head to p  
        while(p->next != NULL){ 
            p = p->next;//traverse the list until p is the last node.The last node always points to NULL. 
        } 
        p->next = temp;//Point the previous last node to the new node created. 
    } 
    return head; 
} 
void printList(node ptr) 
{ 
while (ptr != NULL) 
{ 
printf("%d->", ptr->data); 
ptr = ptr->next; 
} 
printf("NULL\n"); 
} 
int main() 
{ 
    node head=NULL; 
    int i; 
    for(i=0;i<5;i++) 
    { 
        head=addNode(head,i); 
    } 
    printList(head); 
    printf("Hello World"); 
    return 0; 
} 

 

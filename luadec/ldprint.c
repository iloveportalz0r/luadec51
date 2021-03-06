/* luadec, based on luac */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>

#define DEBUG_PRINT

#ifndef LUA_OPNAMES
#define LUA_OPNAMES
#endif

#include "ldebug.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lundump.h"
#include "lstring.h"

#include "StringBuffer.h"
#include "proto.h"

#include "print.h"
#include "structs.h"

#define stddebug stdout

extern int locals;
extern int localdeclare[255][255];
extern int functionnum;
extern lua_State* glstate;
extern int guess_locals;

char* error_nil = "ERROR_nil";
char* nilstr = "nil";
char* upvalue = "upvaluexxxxxxxxx";
StringBuffer *errorStr;

/*
 * -------------------------------------------------------------------------
 */

char* getupval(Function * F, int r)
{
	if(F->f->upvalues && r < F->f->sizeupvalues)
	{
		return (char*)getstr(F->f->upvalues[r]);
	}
	else
	{
		char* s = malloc(20);
		sprintf(s, "upvalue_%d", r);
		return s;
	}
}

#define GLOBAL(r) (char*)svalue(&f->k[r])
#define UPVALUE(r) ( getupval(F,r) )
#define REGISTER(r) F->R[r]
#define PRIORITY(r) (r>=MAXSTACK ? 0 : F->Rprio[r])
#define LOCAL(r) (F->f->locvars ? ((char*)getstr(F->f->locvars[r].varname)) : error_nil)
#define LOCAL_STARTPC(r) F->f->locvars[r].startpc
#define PENDING(r) F->Rpend[r]
#define CALL(r) F->Rcall[r]
#define IS_TABLE(r) F->Rtabl[r]
#define IS_VARIABLE(r) F->Rvar[r]
#define IS_CONSTANT(r) (r >= 256) // TODO: Lua5.1 specific. Should use MSR!!!!

#define SET_CTR(s) s->ctr
#define SET(s,y) s->values[y]
#define SET_IS_EMPTY(s) (s->ctr == 0)

#define opstr(o) ((o)==OP_EQ?"==":(o)==OP_LE?"<=":(o)==OP_LT?"<":(((o)==OP_TEST)||((o)==OP_TESTSET))?NULL:"?") // Lua5.1 specific
#define invopstr(o) ((o)==OP_EQ?"~=":(o)==OP_LE?">":(o)==OP_LT?">=":(((o)==OP_TEST)||((o)==OP_TESTSET))?"not":"?") // Lua5.1 specific

#define IsMain(f)	(f->linedefined==0)
#define fb2int(x)	(((x) & 7) << ((x) >> 3))

#define SET_ERROR(F,e) { StringBuffer_printf(errorStr," -- DECOMPILER ERROR: %s\n", (e)); RawAddStatement((F),errorStr); }

static int debug;

static char* error;
static int errorCode;

void RawAddStatement(Function * F, StringBuffer * str);
void DeclareLocal(Function * F, int ixx, char* value);

Statement *NewStatement(char *code, int line, int indent)
{
	Statement *self;
	self = calloc(sizeof(Statement), 1);
	cast(ListItem*, self)->next = NULL;
	self->code = code;
	self->line = line;
	self->indent = indent;
	return self;
}

void DeleteStatement(Statement * self, void* dummy)
{
	free(self->code);
}

void PrintStatement(Statement * self, void* F_)
{
	int i;
	Function* F = cast(Function*, F_);

	for(i = 0; i < self->indent; ++i)
	{
		StringBuffer_add(F->decompiledCode, "\t");
	}
	StringBuffer_addPrintf(F->decompiledCode, "%s\n", self->code);
}

LogicExp* MakeExpNode(BoolOp* boolOp)
{
	LogicExp* node = cast(LogicExp*, malloc(sizeof(LogicExp)));
	node->parent = NULL;
	node->subexp = NULL;
	node->next = NULL;
	node->prev = NULL;
	node->op1 = boolOp->op1;
	node->op2 = boolOp->op2;
	node->op = boolOp->op;
	node->dest = boolOp->dest;
	node->neg = boolOp->neg;
	node->is_chain = 0;
	return node;
}

LogicExp* MakeExpChain(int dest)
{
	LogicExp* node = cast(LogicExp*, malloc(sizeof(LogicExp)));
	node->parent = NULL;
	node->subexp = NULL;
	node->next = NULL;
	node->prev = NULL;
	node->dest = dest;
	node->is_chain = 1;
	return node;
}

StringBuffer* PrintLogicItem(StringBuffer* str, LogicExp* exp, int inv, int rev)
{
	if(exp->subexp)
	{
		StringBuffer_addChar(str, '(');
		str = PrintLogicExp(str, exp->dest, exp->subexp, inv, rev);
		StringBuffer_addChar(str, ')');
	}
	else
	{
		char *op;
		int cond = exp->neg;
		if(inv) cond = !cond;
		if(rev) cond = !cond;
		if(cond)
			op = invopstr(exp->op);
		else
			op = opstr(exp->op);
		if((exp->op != OP_TEST) && (exp->op != OP_TESTSET))
		{
			StringBuffer_addPrintf(str, "%s %s %s", exp->op1, op, exp->op2);
		}
		else
		{
			if(op)
				StringBuffer_addPrintf(str, "%s %s", op, exp->op2);
			else
				StringBuffer_addPrintf(str, "%s", exp->op2);
		}
	}
	return str;
}

StringBuffer* PrintLogicExp(StringBuffer* str, int dest, LogicExp* exp, int inv_, int rev)
{
	int inv = inv_;
	if(!str)
		str = StringBuffer_new(NULL);
	while(exp->next)
	{
		char* op;
		int cond = exp->dest > dest;
		inv = cond ? inv_ : !inv_;
		str = PrintLogicItem(str, exp, inv, rev);
		exp = exp->next;
		if(inv_) cond = !cond;
		if(rev) cond = !cond;
		op = cond ? "and" : "or";
		StringBuffer_addPrintf(str, " %s ", op);
	}
	return PrintLogicItem(str, exp, inv_, rev);
}

void TieAsNext(LogicExp* curr, LogicExp* item)
{
	curr->next = item;
	item->prev = curr;
	item->parent = curr->parent;
}

void Untie(LogicExp* curr, int* thenaddr)
{
	LogicExp* previous = curr->prev;
	if(previous)
		previous->next = NULL;
	curr->prev = NULL;
	curr->parent = NULL;
}


void TieAsSubExp(LogicExp* parent, LogicExp* item)
{
	parent->subexp = item;
	while(item)
	{
		item->parent = parent;
		item = item->next;
	}
}

LogicExp* MakeBoolean(Function * F, int* endif, int* thenaddr)
{
	int i;
	int firstaddr, elseaddr, last, realLast;
	LogicExp *curr, *first;
	int dest;

	if(endif)
		*endif = 0;

	if(F->nextBool == 0)
	{
		SET_ERROR(F, "Attempted to build a boolean expression without a pending context");
		return NULL;
	}

	realLast = F->nextBool - 1;
	last = realLast;
	firstaddr = F->bools[0]->pc + 2;
	*thenaddr = F->bools[last]->pc + 2;
	elseaddr = F->bools[last]->dest;

	for(i = realLast; i >= 0; --i)
	{
		int dest = F->bools[i]->dest;
		if((elseaddr > *thenaddr) &&
				(((F->bools[i]->op == OP_TEST) || (F->bools[i]->op == OP_TESTSET)) ? (dest > elseaddr + 1) :
				 (dest > elseaddr)))
		{
			last = i;
			*thenaddr = F->bools[i]->pc + 2;
			elseaddr = dest;
		}
	}

	{
		int tmpLast = last;
		for(i = 0; i < tmpLast; ++i)
		{
			int dest = F->bools[i]->dest;
			if(elseaddr > firstaddr)
			{
				if(dest < firstaddr)
				{
					last = i;
					*thenaddr = F->bools[i]->pc + 2;
					elseaddr = dest;
				}
			}
			else
			{
				if(dest == firstaddr)
				{
					last = i;
					*thenaddr = F->bools[i]->pc + 2;
					elseaddr = dest;
				}
				else
				{
					break;
				}
			}
		}
	}

	dest = F->bools[0]->dest;
	curr = MakeExpNode(F->bools[0]);

	if(dest > firstaddr && dest <= *thenaddr)
	{
		first = MakeExpChain(dest);
		TieAsSubExp(first, curr);
	}
	else
	{
		first = curr;
		if(endif)
			*endif = dest;
	}

	if(debug)
	{
		printf("\n");
		for(i = 0; i <= last; ++i)
		{
			BoolOp* op = F->bools[i];
			if(debug)
			{
				printf("Exps(%d): at %d\tdest %d\tneg %d\t(%s %s %s) cpd %d \n", i,
					   op->pc, op->dest, op->neg, op->op1, opstr(op->op), op->op2, curr->parent ? curr->parent->dest : -1);
			}
		}
		printf("\n");
	}

	for(i = 1; i <= last; ++i)
	{
		BoolOp* op = F->bools[i];
		int at = op->pc;
		int dest = op->dest;

		LogicExp* exp = MakeExpNode(op);
		if(dest < firstaddr)
		{
			/* jump to loop in a while */
			TieAsNext(curr, exp);
			curr = exp;
			if(endif)
				*endif = dest;
		}
		else if(dest > *thenaddr)
		{
			/* jump to "else" */
			TieAsNext(curr, exp);
			curr = exp;
			if(endif)
			{
				if((op->op != OP_TEST) && (op->op != OP_TESTSET))
				{
					if(*endif != 0 && *endif != dest)
					{
						SET_ERROR(F, "unhandled construct in 'if'");
						//return NULL;
					}
				}
				*endif = dest;
			}
		}
		else if(dest == curr->dest)
		{
			/* within current chain */
			TieAsNext(curr, exp);
			curr = exp;
		}
		else if(dest > curr->dest)
		{
			if(curr->parent == NULL || dest < curr->parent->dest)
			{
				/* creating a new level */
				LogicExp* subexp = MakeExpChain(dest);
				TieAsNext(curr, exp);
				curr = exp;
				if(curr->parent == NULL)
				{
					TieAsSubExp(subexp, first);
					first = subexp;
				}
			}
			else if(dest > curr->parent->dest)
			{
				/* start a new chain */
				LogicExp* prevParent;
				LogicExp* chain;
				TieAsNext(curr, exp);
				curr = curr->parent;
				if(!curr->is_chain)
				{
					SET_ERROR(F, "unhandled construct in 'if'");
					return NULL;
				};
				prevParent = curr->parent;
				chain = MakeExpChain(dest);
				Untie(curr, thenaddr);
				if(prevParent)
					if(prevParent->is_chain)
						prevParent = prevParent->subexp;
				TieAsSubExp(chain, curr);

				curr->parent = prevParent;
				if(prevParent == NULL)
				{
					first = chain;
				}
				else
				{
					// todo
					TieAsNext(prevParent, chain);
				}
			}
		}
		else if(dest > firstaddr && dest < curr->dest)
		{
			/* start a new chain */
			LogicExp* subexp = MakeExpChain(dest);
			TieAsSubExp(subexp, exp);
			TieAsNext(curr, subexp);
			curr = exp;
		}
		else
		{
			SET_ERROR(F, "unhandled construct in 'if'");
			return NULL;
		}

		if(curr->parent && at + 3 > curr->parent->dest)
		{
			curr->parent->dest = curr->dest;
			if(i < last)
			{
				LogicExp* chain = MakeExpChain(curr->dest);
				TieAsSubExp(chain, first);
				first = chain;
			}
			curr = curr->parent;
		}
	}
	if(first->is_chain)
		first = first->subexp;
	for(i = last + 1; i < F->nextBool; ++i)
		F->bools[i-last-1] = F->bools[i];
	if(!F->bools[0])
		F->bools[0] = calloc(sizeof(BoolOp), 1);
	F->nextBool -= last + 1;
	if(endif)
		if(*endif == 0)
		{
			*endif = *thenaddr;
		}
	return first;
}

char* WriteBoolean(LogicExp* exp, int* thenaddr, int* endif, int test)
{
	char* result;
	StringBuffer* str;

	if(exp)
	{
		str = PrintLogicExp(NULL, *thenaddr, exp, 0, test);
		if(test && endif && *endif == 0)
		{
			//SET_ERROR(F,"Unhandled construct in boolean test");
			result = malloc(30);
			sprintf(result, " --UNHANDLEDCONTRUCT-- ");
			return result;
		}
	}
	else
	{
		result = malloc(30);
		sprintf(result, "error_maybe_false");
		return result;
	}

	result = StringBuffer_getBuffer(str);
	StringBuffer_delete(str);
	return result;
}

void FlushElse(Function* F);

char* OutputBoolean(Function* F, int* endif, int test)
{
	int thenaddr;
	char* result;
	LogicExp* exp;

	FlushElse(F);
	if(error) return NULL;
	exp = MakeBoolean(F, endif, &thenaddr);
	if(error) return NULL;
	result = WriteBoolean(exp, &thenaddr, endif, test);
	if(error) return NULL;
	return result;
}

void StoreEndifAddr(Function * F, int addr)
{
	Endif* at = F->nextEndif;
	Endif* prev = NULL;
	Endif* newEndif = malloc(sizeof(Endif));
	newEndif->addr = addr;
	while(at && at->addr < addr)
	{
		prev = at;
		at = at->next;
	}
	if(!prev)
	{
		newEndif->next = F->nextEndif;
		F->nextEndif = newEndif;
	}
	else
	{
		newEndif->next = at;
		prev->next = newEndif;
	}
	if(debug)
	{
		printf("Stored at endif list: ");
		for(at = F->nextEndif; at != NULL; at = at->next)
		{
			if(at == newEndif)
				printf("<%d> ", at->addr);
			else
				printf("%d ", at->addr);
		}
		printf("\n");
	}
}

int PeekEndifAddr(Function* F, int addr)
{
	Endif* at = F->nextEndif;
	while(at)
	{
		if(at->addr == addr)
			return 1;
		else if(at->addr > addr)
			break;
		at = at->next;
	}
	return 0;
}

int GetEndifAddr(Function* F, int addr)
{
	Endif* at = F->nextEndif;
	Endif* prev = NULL;
	while(at)
	{
		if(at->addr == addr)
		{
			if(prev)
				prev->next = at->next;
			else
				F->nextEndif = at->next;
			free(at);
			return 1;
		}
		else if(at->addr > addr)
			break;
		prev = at;
		at = at->next;
	}
	return 0;
}

void BackpatchStatement(Function * F, char * code, int line)
{
	ListItem *walk = F->statements.head;
	while(walk)
	{
		Statement* stmt = (Statement*) walk;
		walk = walk->next;
		if(stmt->backpatch && stmt->line == line)
		{
			free(stmt->code);
			stmt->code = code;
			return;
		}
	}
	SET_ERROR(F, "Confused while interpreting a jump as a 'while'");
}

void RawAddStatement(Function * F, StringBuffer * str)
{
	char *copy;
	Statement* stmt;
	copy = StringBuffer_getCopy(str);
	if(F->released_local)
	{
		int i = 0;
		int lpc = F->released_local;
		char* scopeclose[] =
		{
			"end", "else", "elseif", "while", "until", NULL
		};
		F->released_local = 0;
		for(i = 0; scopeclose[i]; ++i)
		{
			if(strstr(copy, scopeclose[i]) == copy)
			{
				break;
			}
		}
		if(!scopeclose[i])
		{
			int added = 0;
			Statement* stmt = cast(Statement*, F->statements.head);
			Statement* prev = NULL;
			Statement* newst;
			while(stmt)
			{
				if(!added)
				{
					if(stmt->line >= lpc)
					{
						Statement *newst = NewStatement(strdup("do"), lpc, stmt->indent);
						if(prev)
						{
							prev->super.next = cast(ListItem*, newst);
							newst->super.next = cast(ListItem*, stmt);
						}
						else
						{
							F->statements.head = cast(ListItem*, newst);
							newst->super.next = cast(ListItem*, stmt);
						}
						added = 1;
						++stmt->indent;
					}
				}
				else
				{
					++stmt->indent;
				}
				prev = stmt;
				stmt = cast(Statement*, stmt->super.next);
			}
			newst = NewStatement(strdup("end"), F->pc, F->indent);
			AddToList(&(F->statements), cast(ListItem*, newst));
		}
	}
	stmt = NewStatement(copy, F->pc, F->indent);
	AddToList(&(F->statements), cast(ListItem*, stmt));
	F->lastLine = F->pc;
}

void FlushBoolean(Function * F)
{
	FlushElse(F);
	while(F->nextBool > 0)
	{
		char* test;
		int endif;
		int thenaddr;
		StringBuffer* str = StringBuffer_new(NULL);
		LogicExp* exp = MakeBoolean(F, &endif, &thenaddr);
		if(error) return;
		if(endif < F->pc - 1)
		{
			test = WriteBoolean(exp, &thenaddr, &endif, 1);
			if(error) return;
			StringBuffer_printf(str, "while %s do", test);
			/* verify this '- 2' */
			BackpatchStatement(F, StringBuffer_getBuffer(str), endif - 2);
			if(error) return;
			--F->indent;
			StringBuffer_add(str, "end");
			RawAddStatement(F, str);
		}
		else
		{
			test = WriteBoolean(exp, &thenaddr, &endif, 0);
			if(error) return;
			StoreEndifAddr(F, endif);
			StringBuffer_addPrintf(str, "if %s then", test);
			F->elseWritten = 0;
			RawAddStatement(F, str);
			++F->indent;
		}
		StringBuffer_delete(str);
	}
	F->testpending = 0;
}

void AddStatement(Function * F, StringBuffer * str)
{
	FlushBoolean(F);
	if(error)
	{
		return;
	}
	RawAddStatement(F, str);
}

void MarkBackpatch(Function* F)
{
	Statement* stmt = (Statement*) LastItem(&(F->statements));
	stmt->backpatch = 1;
}

void FlushElse(Function* F)
{
	if(F->elsePending > 0)
	{
		StringBuffer* str = StringBuffer_new(NULL);
		int fpc = F->bools[0]->pc;
		/* Should elseStart be a stack? */
		if(F->nextBool > 0 && (fpc == F->elseStart || fpc - 1 == F->elseStart))
		{
			char* test;
			int endif;
			int thenaddr;
			LogicExp* exp;
			exp = MakeBoolean(F, &endif, &thenaddr);
			if(error) return;
			test = WriteBoolean(exp, &thenaddr, &endif, 0);
			if(error) return;
			StoreEndifAddr(F, endif);
			StringBuffer_addPrintf(str, "elseif %s then", test);
			F->elseWritten = 0;
			RawAddStatement(F, str);
			++F->indent;
		}
		else
		{
			StringBuffer_printf(str, "else");
			RawAddStatement(F, str);
			/* this test circumvents jump-to-jump optimization at
			   the end of if blocks */
			if(!PeekEndifAddr(F, F->pc + 3))
			{
				StoreEndifAddr(F, F->elsePending);
			}
			++F->indent;
			F->elseWritten = 1;
		}
		F->elsePending = 0;
		F->elseStart = 0;
		StringBuffer_delete(str);
	}
}

/*
 * -------------------------------------------------------------------------
 */

DecTableItem *NewTableItem(char *value, int num, char *key)
{
	DecTableItem *self = calloc(sizeof(DecTableItem), 1);
	((ListItem *) self)->next = NULL;
	self->value = strdup(value);
	self->numeric = num;
	if(key)
		self->key = strdup(key);
	else
		self->key = NULL;
	return self;
}

/*
 * -------------------------------------------------------------------------
 */

void DeclarePendingLocals(Function * F);

void Assign(Function * F, char* dest, char* src, int reg, int prio, int mayTest)
{
	char* nsrc = src ? strdup(src) : NULL;

	if(PENDING(reg))
	{
		if(guess_locals)
		{
			char tmp[32];
			sprintf(tmp, "Overwrote pending register: %i", reg);
			SET_ERROR(F, tmp);
		}
		else
		{
			char *s;
			SET_ERROR(F, "Overwrote pending register. Missing locals? Creating them.");
			s = strdup(REGISTER(reg));
			DeclareLocal(F, reg, s);
		}
		return;
	}

	if(reg != -1)
	{
		PENDING(reg) = 1;
		CALL(reg) = 0;
		F->Rprio[reg] = prio;
	}

	if(debug)
	{
		printf("SET_CTR(Tpend) = %d \n", SET_CTR(F->tpend));
	}

	if(reg != -1 && F->testpending == reg + 1 && mayTest && F->testjump == F->pc + 2)
	{
		int endif;
		StringBuffer* str = StringBuffer_new(NULL);
		char* test = OutputBoolean(F, &endif, 1);
		if(error)
		{
			return;
		}
		if(endif >= F->pc)
		{
			StringBuffer_printf(str, "%s or %s", test, src);
			free(nsrc);
			nsrc = StringBuffer_getBuffer(str);
			free(test);
			StringBuffer_delete(str);
			F->testpending = 0;
			F->Rprio[reg] = 8;
		}
	}
	F->testjump = 0;

	if(reg != -1 && !IS_VARIABLE(reg))
	{
		if(REGISTER(reg))
		{
			free(REGISTER(reg));
		}
		REGISTER(reg) = nsrc;
		AddToSet(F->tpend, reg);
	}
	else
	{
		AddToVarStack(F->vpend, strdup(dest), nsrc, reg);
	}
}

int MatchTable(DecTable * tbl, int *name)
{
	return tbl->reg == *name;
}

void DeleteTable(DecTable * tbl)
{
	/*
	 * TODO: delete values from table
	 */
	free(tbl);
}

void CloseTable(Function * F, int r)
{
	DecTable *tbl = (DecTable *) PopFromList(&(F->tables));
	if(tbl->reg != r)
	{
		SET_ERROR(F, "Unhandled construct in table");
		return;
	}
	DeleteTable(tbl);
	F->Rtabl[r] = 0;
}

char *PrintTable(Function * F, int r, int returnCopy)
{
	char *result = NULL;
	StringBuffer *str = StringBuffer_new("{");
	DecTable *tbl = (DecTable*)FindInList(&(F->tables), (ListItemCmpFn)MatchTable, &r);
	int numerics = 0;
	DecTableItem *item = (DecTableItem*)tbl->numeric.head;
	if(item)
	{
		StringBuffer_add(str, item->value);
		item = (DecTableItem *) item->super.next;
		numerics = 1;
		while(item)
		{
			StringBuffer_add(str, ", ");
			StringBuffer_add(str, item->value);
			item = (DecTableItem *) item->super.next;
		}
	}
	item = (DecTableItem *) tbl->keyed.head;
	if(item)
	{
		int first;
		if(numerics)
			StringBuffer_add(str, "; ");
		first = 1;
		while(item)
		{
			char* key = item->key;
			if(first)
				first = 0;
			else
				StringBuffer_add(str, ", ");
			if(key[0] == '\"')
			{
				char* last = strrchr(key, '\"');
				*last = '\0';
				++key;
			}
			StringBuffer_addPrintf(str, "%s = %s", key, item->value);
			item = (DecTableItem *) item->super.next;
		}
	}
	StringBuffer_addChar(str, '}');
	PENDING(r) = 0;
	Assign(F, REGISTER(r), StringBuffer_getRef(str), r, 0, 0);
	if(error)
	{
		return NULL;
	}
	if(returnCopy)
	{
		result = StringBuffer_getCopy(str);
	}
	StringBuffer_delete(str);
	CloseTable(F, r);
	if(error)
	{
		return NULL;
	}
	return result;
}


DecTable *NewTable(int r, Function * F, int b, int c) // Lua5.1 specific
{
	DecTable *self = calloc(sizeof(DecTable), 1);
	((ListItem *) self)->next = NULL;
	InitList(&(self->numeric));
	InitList(&(self->keyed));
	self->reg = r;
	self->topNumeric = 0;
	self->F = F;
	self->arraySize = fb2int(b);
	self->keyedSize = fb2int(c); //1<<c;
	PENDING(r) = 1;
	return self;
}

void AddToTable(Function* F, DecTable * tbl, char *value, char *key)
{
	DecTableItem *item;
	List *type;
	int index;
	if(key == NULL)
	{
		type = &(tbl->numeric);
		index = tbl->topNumeric;
		++tbl->topNumeric;
	}
	else
	{
		type = &(tbl->keyed);
		++tbl->used;
		index = 0;
	}
	item = NewTableItem(value, index, key);
	AddToList(type, (ListItem *) item);
	// FIXME: should work with arrays, too
	if(tbl->keyedSize == tbl->used && tbl->arraySize == 0)
	{
		PrintTable(F, tbl->reg, 0);
		if(error)
		{
			return;
		}
	}
}

void StartTable(Function * F, int r, int b, int c)
{
	DecTable *tbl = NewTable(r, F, b, c);
	AddToList(&(F->tables), (ListItem*)tbl);
	F->Rtabl[r] = 1;
	F->Rtabl[r] = 1;
	if(b == 0 && c == 0)
	{
		PrintTable(F, r, 1);
		if(error)
		{
			return;
		}
	}
}

void SetList(Function * F, int a, int b, int c)
{
	int i;
	DecTable *tbl = (DecTable*)LastItem(&(F->tables));
	if(tbl == NULL)
	{
		tbl = NewTable(a, F, b, c);
		AddToList(&(F->tables), (ListItem*)tbl);
	}
	else if(tbl->reg != a)
	{
		SET_ERROR(F, "Unhandled construct in list.");
		return;
	}
	for(i = 1; i <= b; ++i)
	{
		char* rstr = GetR(F, a + i);
		if(error)
		{
			return;
		}
		AddToTable(F, tbl, rstr, NULL); // Lua5.1 specific TODO: it's not really this :(
		if(error)
		{
			return;
		}
	}
	PrintTable(F, tbl->reg, 0);
	if(error)
	{
		return;
	}
}

void UnsetPending(Function * F, int r)
{
	if(!IS_VARIABLE(r))
	{
		if(!PENDING(r) && !CALL(r))
		{
			if(guess_locals)
			{
				SET_ERROR(F, "Confused about usage of registers!");
			}
			else
			{
				char *s;
				SET_ERROR(F, "Confused about usage of registers, missing locals? Creating them");
				s = strdup(REGISTER(r));
				DeclareLocal(F, r, s);
			}
			return;
		}
		PENDING(r) = 0;
		RemoveFromSet(F->tpend, r);
	}
}

int SetTable(Function * F, int a, char *bstr, char *cstr)
{
	DecTable *tbl = (DecTable*)LastItem(&(F->tables));
	if((!tbl) || (tbl->reg != a))
	{
		/*
		 * SetTable is not being applied to the table being generated. (This
		 * will probably need a more strict check)
		 */
		UnsetPending(F, a);
		if(error) return 0;
		return 0;
	}
	AddToTable(F, tbl, cstr, bstr);
	if(error) return 0;
	return 1;
}

/*
 * -------------------------------------------------------------------------
 */

Function *NewFunction(const Proto * f)
{
	Function *self;

	/*
	 * calloc, to ensure all parameters are 0/NULL
	 */
	self = calloc(sizeof(Function), 1);
	InitList(&(self->statements));
	self->f = f;
	self->vpend = calloc(sizeof(VarStack), 1);
	self->tpend = calloc(sizeof(IntSet), 1);
	self->whiles = calloc(sizeof(IntSet), 1);
	self->repeats = calloc(sizeof(IntSet), 1);
	self->repeats->mayRepeat = 1;
	self->untils = calloc(sizeof(IntSet), 1);
	self->do_opens = calloc(sizeof(IntSet), 1);
	self->do_closes = calloc(sizeof(IntSet), 1);
	self->decompiledCode = StringBuffer_new(NULL);
	self->bools[0] = calloc(sizeof(BoolOp), 1);
	self->intspos = 0;
	return self;
}

void DeleteFunction(Function * self)
{
	int i;
	LoopList(&(self->statements), (ListItemFn) DeleteStatement, NULL);
	/*
	 * clean up registers
	 */
	for(i = 0; i < MAXARG_A; ++i)
	{
		if(self->R[i])
			free(self->R[i]);
	}
	StringBuffer_delete(self->decompiledCode);
	free(self->vpend);
	free(self->tpend);
	free(self->whiles);
	free(self->repeats);
	free(self->untils);
	free(self->do_opens);
	free(self->do_closes);
	free(self);
}

char *GetR(Function * F, int r)
{
	if(IS_TABLE(r))
	{
		PrintTable(F, r, 0);
		if(error)
		{
			return NULL;
		}
	}
	UnsetPending(F, r);
	if(error)
	{
		return NULL;
	}
	return F->R[r];
}

void DeclareVariable(Function * F, const char *name, int reg)
{
	F->Rvar[reg] = 1;
	if(F->R[reg])
	{
		free(F->R[reg]);
	}
	F->R[reg] = strdup(name);
	F->Rprio[reg] = 0;
	UnsetPending(F, reg);
	if(error)
	{
		return;
	}
}

void OutputAssignments(Function * F)
{
	int i, srcs, size;
	StringBuffer *vars;
	StringBuffer *exps;
	if(!SET_IS_EMPTY(F->tpend))
	{
		return;
	}
	vars = StringBuffer_new(NULL);
	exps = StringBuffer_new(NULL);
	size = SET_CTR(F->vpend);
	srcs = 0;
	for(i = 0; i < size; ++i)
	{
		int r = F->vpend->regs[i];
		if(!(r == -1 || PENDING(r)))
		{
			SET_ERROR(F, "Attempted to generate an assignment, but got confused about usage of registers");
			return;
		}

		if(i > 0)
			StringBuffer_prepend(vars, ", ");
		StringBuffer_prepend(vars, F->vpend->dests[i]);

		if(F->vpend->srcs[i] && (srcs > 0 || (srcs == 0 && strcmp(F->vpend->srcs[i], "nil") != 0) || i == size - 1))
		{
			if(srcs > 0)
				StringBuffer_prepend(exps, ", ");
			StringBuffer_prepend(exps, F->vpend->srcs[i]);
			++srcs;
		}
	}

	for(i = 0; i < size; ++i)
	{
		int r = F->vpend->regs[i];
		if(r != -1)
			PENDING(r) = 0;
		free(F->vpend->dests[i]);
		if(F->vpend->srcs[i])
			free(F->vpend->srcs[i]);
	}
	F->vpend->ctr = 0;

	if(i > 0)
	{
		StringBuffer_add(vars, " = ");
		StringBuffer_add(vars, StringBuffer_getRef(exps));
		AddStatement(F, vars);
		if(error)
			return;
	}
	StringBuffer_delete(vars);
	StringBuffer_delete(exps);
}

void ReleaseLocals(Function * F)
{
	int i;
	for(i = F->f->sizelocvars - 1; i >= 0 ; --i)
	{
		if(F->f->locvars[i].endpc == F->pc)
		{
			int r;
			--F->freeLocal;
			r = F->freeLocal;
			//fprintf(stderr,"%d %d %d\n",i,r, F->pc);
			if(!IS_VARIABLE(r))
			{
				// fprintf(stderr,"--- %d %d\n",i,r);
				SET_ERROR(F, "Confused about usage of registers for local variables.");
				return;
			}
			F->Rvar[r] = 0;
			F->Rprio[r] = 0;
			if(!F->ignore_for_variables && !F->released_local)
			{
				F->released_local = F->f->locvars[i].startpc;
			}
		}
	}
	F->ignore_for_variables = 0;
}

void DeclareLocals(Function * F)
{
	int i;
	int internalLocals = 0;
	StringBuffer *str;
	StringBuffer *rhs;
	char *names[MAXARG_A];
	int startparams = 0;
	/*
	 * Those are declaration of parameters.
	 */
	if(F->pc == 0)
	{
		startparams = F->f->numparams + (F->f->is_vararg & 2);
	}
	str = StringBuffer_new("local ");
	rhs = StringBuffer_new(" = ");
	int locals = 0;
	if(F->pc != 0)
	{
		for(i = startparams; i < F->f->maxstacksize; ++i)
		{
			if(localdeclare[functionnum][i] == F->pc)
			{
				char* name;
				int r = i;
				name = malloc(10);
				sprintf(name, "l_%d_%d", functionnum, i);
				if(F->internal[r])
				{
					names[r] = name;
					F->internal[r] = 0;
					++internalLocals;
					continue;
				}
				if(PENDING(r))
				{
					if(locals > 0)
					{
						StringBuffer_add(str, ", ");
						StringBuffer_add(rhs, ", ");
					}
					StringBuffer_add(str, name);
					StringBuffer_add(rhs, GetR(F, r));
				}
				else
				{
					StringBuffer_add(str, ", ");
					StringBuffer_add(str, name);
				}
				CALL(r) = 0;
				IS_VARIABLE(r) = 1;
				names[r] = name;
				++locals;
			}
		}
	}
	for(i = startparams; i < F->f->sizelocvars; ++i)
	{
		if(F->f->locvars[i].startpc == F->pc)
		{
			int r = F->freeLocal + locals + internalLocals;
			Instruction instr = F->f->code[F->pc];
			// handle FOR loops
			if(GET_OPCODE(instr) == OP_FORPREP)
			{
				F->f->locvars[i].startpc = F->pc + 1;
				continue;
			}
			// handle TFOR loops
			if(GET_OPCODE(instr) == OP_JMP)
			{
				Instruction n2 = F->f->code[F->pc+1+GETARG_sBx(instr)];
				//fprintf(stderr,"3 %d\n",F->pc+1+GETARG_sBx(instr));
				//fprintf(stderr,"4 %s %d\n",luaP_opnames[GET_OPCODE(n2)], F->pc+GETARG_sBx(instr));
				if(GET_OPCODE(n2) == OP_TFORLOOP)
				{
					F->f->locvars[i].startpc = F->pc + 1;
					continue;
				}
			}
			if((F->internal[r]))
			{
				names[r] = LOCAL(i);
				PENDING(r) = 0;
				IS_VARIABLE(r) = 1;
				F->internal[r] = 0;
				++internalLocals;
				continue;
			}
			if(PENDING(r))
			{
				if(locals > 0)
				{
					StringBuffer_add(str, ", ");
					StringBuffer_add(rhs, ", ");
				}
				StringBuffer_add(str, LOCAL(i));
				StringBuffer_add(rhs, GetR(F, r));
				if(error)
				{
					return;
				}
			}
			else
			{
				if(!(locals > 0))
				{
					SET_ERROR(F, "Confused at declaration of local variable");
					StringBuffer_prepend(str, "-- "); // comment out broken line
				}
				StringBuffer_add(str, ", ");
				StringBuffer_add(str, LOCAL(i));
			}
			CALL(r) = 0;
			IS_VARIABLE(r) = 1;
			names[r] = LOCAL(i);
			++locals;
		}
	}
	if(locals > 0)
	{
		StringBuffer_add(str, StringBuffer_getRef(rhs));
		AddStatement(F, str);
		if(error)
		{
			return;
		}
	}
	StringBuffer_delete(rhs);
	StringBuffer_prune(str);
	for(i = 0; i < locals + internalLocals; ++i)
	{
		int r = F->freeLocal + i;
		DeclareVariable(F, names[r], r);
		if(error)
		{
			return;
		}
	}
	F->freeLocal += locals + internalLocals;
}

char* PrintFunction(Function * F)
{
	char* result;
	StringBuffer_prune(F->decompiledCode);
	LoopList(&(F->statements), (ListItemFn) PrintStatement, F);
	result = StringBuffer_getBuffer(F->decompiledCode);
	return result;
}

/*
 * -------------------------------------------------------------------------
 */

static char *operators[22] =
{
	" ", " ", " ", " ", " ", " ", " ", " ", " ", " ", " ", " ",
	"+", "-", "*", "/", "%", "^", "-", "not ", "#", ".." // Lua5.1 specific
};

static int priorities[22] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 4, 3, 3, 3, 1, 2, 2, 2, 5 };  // Lua5.1 specific

char *RegisterOrConstant(Function * F, int r)
{
	if(IS_CONSTANT(r))
	{
		return DecompileConstant(F->f, r - 256); // TODO: Lua5.1 specific. Should change to MSR!
	}
	else
	{
		char *reg = GetR(F, r);
		if(error)
		{
			return NULL;
		}
		if(reg == NULL)
		{
			reg = "DECOMPILE_ERROR"; // TODO: Fix this
		}
		char *copy = malloc(strlen(reg) + 1);
		strcpy(copy, reg);
		return copy;
	}
}

void MakeIndex(Function * F, StringBuffer * str, char* rstr, int self)
{
	int dot = 0;
	/*
	 * see if index can be expressed without quotes
	 */
	if(rstr[0] == '\"')
	{
		if(isalpha(rstr[1]) || rstr[1] == '_')
		{
			char *at = rstr + 1;
			dot = 1;
			while(*at != '"')
			{
				if(!isalnum(*at) && *at != '_')
				{
					dot = 0;
					break;
				}
				++at;
			}
		}
	}
	if(dot)
	{
		++rstr;
		rstr[strlen(rstr) - 1] = '\0';
		if(self)
		{
			StringBuffer_addPrintf(str, ":%s", rstr);
		}
		else
		{
			StringBuffer_addPrintf(str, ".%s", rstr);
		}
		--rstr;
	}
	else
	{
		StringBuffer_addPrintf(str, "[%s]", rstr);
	}
}

void FunctionHeader(Function * F)
{
	int saveIndent = F->indent;
	const Proto* f = F->f;
	StringBuffer* str = StringBuffer_new(NULL);
	F->indent = 0;
	if(f->numparams > 0)
	{
		int i;
		StringBuffer_addPrintf(str, "(");
		for(i = 0; i < f->numparams - 1; ++i)
		{
			StringBuffer_addPrintf(str, "l_%d_%d, ", functionnum, i);
		}
		StringBuffer_addPrintf(str, "l_%d_%d", functionnum, i);
		if(f->is_vararg)
		{
			StringBuffer_add(str, ", ...");
		}
		StringBuffer_addPrintf(str, ")");
		AddStatement(F, str);
		if(error)
		{
			return;
		}
		StringBuffer_prune(str);
	}
	else if(!IsMain(f))
	{
		if(f->is_vararg)
		{
			StringBuffer_add(str, "(...)");
		}
		else
		{
			StringBuffer_add(str, "()");
		}
		AddStatement(F, str);
		if(error)
			return;
		StringBuffer_prune(str);
	}
	F->indent = saveIndent;
	if(!IsMain(f))
	{
		++F->indent;
	}
	StringBuffer_delete(str);
}

void ShowState(Function * F)
{
	int i;
	fprintf(stddebug, "\n");
	fprintf(stddebug, "next bool: %d\n", F->nextBool);
	fprintf(stddebug, "locals(%d): ", F->freeLocal);
	for(i = 0; i <= F->freeLocal; ++i)
	{
		if(i != F->freeLocal)
		{
			fprintf(stddebug, "%d{%s} ", i, REGISTER(i));
		}
		else
		{
			fprintf(stddebug, "%d{%s}\n", i, REGISTER(i));
		}
	}

	fprintf(stddebug, "vpend(%d):", SET_CTR(F->vpend));
	if(SET_CTR(F->vpend) == 0)
	{
		fprintf(stddebug, "\n");
	}
	else
	{
		fprintf(stddebug, " ");
		for(i = 0; i < SET_CTR(F->vpend); ++i)
		{
			int r = F->vpend->regs[i];
			if(r != -1 && !PENDING(r))
			{
				SET_ERROR(F, "Confused about usage of registers for variables");
				return;
			}
			if(i != SET_CTR(F->vpend) - 1)
			{
				fprintf(stddebug, "%d{%s=%s} ", r, F->vpend->dests[i], F->vpend->srcs[i]);
			}
			else
			{
				fprintf(stddebug, "%d{%s=%s}\n", r, F->vpend->dests[i], F->vpend->srcs[i]);
			}
		}
	}

	fprintf(stddebug, "tpend(%d):", SET_CTR(F->tpend));
	if(SET_CTR(F->tpend) == 0)
	{
		fprintf(stddebug, "\n");
	}
	else
	{
		fprintf(stddebug, " ");
		for(i = 0; i < SET_CTR(F->tpend); ++i)
		{
			int r = SET(F->tpend, i);
			if(i != SET_CTR(F->tpend) - 1)
			{
				fprintf(stddebug, "%d{%s} ", r, REGISTER(r));
			}
			else
			{
				fprintf(stddebug, "%d{%s}\n", r, REGISTER(r));
			}
			if(!PENDING(r))
			{
				SET_ERROR(F, "Confused about usage of registers for temporaries");
				return;
			}
		}
	}
}

#define TRY(x)  x; if(error) goto errorHandler

void DeclareLocal(Function * F, int ixx, char* value)
{
	if(!IS_VARIABLE(ixx))
	{
		char* x = malloc(10);
		StringBuffer *str = StringBuffer_new(NULL);

		sprintf(x, "l_%d_%d", functionnum, ixx);
		DeclareVariable(F, x, ixx);
		IS_VARIABLE(ixx) = 1;
		StringBuffer_printf(str, "local %s = %s", x, value);
		RawAddStatement(F, str);
		++F->freeLocal;
		free(str);
	}
}

void DeclarePendingLocals(Function * F)
{
	int i;
	int maxnum = 0;
	int nums[201];
	StringBuffer *str = StringBuffer_new(NULL);
	if(SET_CTR(F->tpend) > 0)
	{
		if(guess_locals)
		{
			StringBuffer_set(str, "-- WARNING: pending registers.");
		}
		else
		{
			StringBuffer_set(str, "-- WARNING: pending registers. Declaring locals.");
			AddStatement(F, str);
			for(i = 0; i < SET_CTR(F->tpend); ++i)
			{
				nums[maxnum] = SET(F->tpend, i);
				++maxnum;
			}
			for(i = 0; i < maxnum; ++i)
			{
				char* s = strdup(REGISTER(nums[i]));
				GetR(F, nums[i]);
				DeclareLocal(F, nums[i], s);
			}
		}
	}
	free(str);
}

char* ProcessCode(const Proto * f, int indent)
{
	int i = 0;

	int ignoreNext = 0;

	/*
	 * State variables for the boolean operations.
	 */
	int boolpending = 0;

	Function *F;
	StringBuffer *str = StringBuffer_new(NULL);

	const Instruction *code = f->code;
	int pc, n = f->sizecode;
	int baseIndent = indent;

	char* output;

	errorStr = StringBuffer_new(NULL);

	F = NewFunction(f);
	F->indent = indent;
	F->pc = 0;
	error = NULL;

	/*
	 * Function parameters are stored in registers from 0 on.
	 */
	for(i = 0; i < f->numparams; ++i)
	{
		char* x = malloc(10);
		sprintf(x, "l_%d_%d", functionnum, i);
		TRY(DeclareVariable(F, x, i));
		IS_VARIABLE(i) = 1;
	}
	F->freeLocal = f->numparams;

	TRY(FunctionHeader(F));

	if(f->is_vararg == 7)
	{
		TRY(DeclareVariable(F, "arg", F->freeLocal));
		++F->freeLocal;
	}

	if((f->is_vararg & 2) && (functionnum != 0))
	{
		++F->freeLocal;
	}

	if(locals)
	{
		for(i = F->freeLocal; i < f->maxstacksize; ++i)
		{
			DeclareLocal(F, i, "nil");
		}
	}

	for(pc = n - 1; pc >= 0; --pc)
	{
		Instruction i = code[pc];
		OpCode o = GET_OPCODE(i);
		if(o == OP_JMP)
		{
			int sbc = GETARG_sBx(i);
			int dest = sbc + pc;
			if(dest < pc)
			{
				if(dest + 2 > 0 && GET_OPCODE(code[dest]) == OP_JMP && !PeekSet(F->whiles, dest))
				{
					AddToSet(F->whiles, dest);
				}
				else if(GET_OPCODE(code[dest]) != OP_FORPREP)
				{
					AddToSet(F->repeats, dest + 2);
					AddToSet(F->untils, pc);
				}
			}
		}
		else if(o == OP_CLOSE)
		{
			int a = GETARG_A(i);
			AddToSet(F->do_opens, f->locvars[a].startpc);
			AddToSet(F->do_closes, f->locvars[a].endpc);
		}
	}

	for(pc = 0; pc < n; ++pc)
	{
		Instruction i = code[pc];
		OpCode o = GET_OPCODE(i);
		int a = GETARG_A(i);
		int b = GETARG_B(i);
		int c = GETARG_C(i);
		int bc = GETARG_Bx(i);
		int sbc = GETARG_sBx(i);
		F->pc = pc;
		// nil optimization of Lua 5.1
		if(pc == 0)
		{
			if((o == OP_SETGLOBAL) || (o == OP_SETUPVAL))
			{
				int ixx;
				for(ixx = F->freeLocal; ixx <= a; ++ixx)
				{
					TRY(Assign(F, REGISTER(ixx), "nil", ixx, 0, 1));
				}
			}
			else if(o != OP_JMP)
			{
				int ixx;
				for(ixx = F->freeLocal; ixx <= a - 1; ++ixx)
				{
					TRY(Assign(F, REGISTER(ixx), "nil", ixx, 0, 1));
				}
			}
		}
		if(ignoreNext)
		{
			--ignoreNext;
			continue;
		}

		/*
		 * Disassembler info
		 */
		if(debug)
		{
			fprintf(stddebug, "----------------------------------------------\n");
			fprintf(stddebug, "\t%d\t", pc + 1);
			fprintf(stddebug, "%-9s\t", luaP_opnames[o]);
			switch(getOpMode(o))
			{
				case iABC:
				{
					fprintf(stddebug, "%d %d %d", a, b, c);
					break;
				}
				case iABx:
				{
					fprintf(stddebug, "%d %d", a, bc);
					break;
				}
				case iAsBx:
				{
					fprintf(stddebug, "%d %d", a, sbc);
					break;
				}
			}
			fprintf(stddebug, "\n");
		}

		TRY(DeclareLocals(F));
		TRY(ReleaseLocals(F));

		while(RemoveFromSet(F->do_opens, pc))
		{
			StringBuffer_set(str, "do");
			TRY(AddStatement(F, str));
			StringBuffer_prune(str);
			++F->indent;
		}

		while(RemoveFromSet(F->do_closes, pc))
		{
			StringBuffer_set(str, "end");
			--F->indent;
			TRY(AddStatement(F, str));
			StringBuffer_prune(str);
		}

		while(GetEndifAddr(F, pc + 1))
		{
			StringBuffer_set(str, "end");
			F->elseWritten = 0;
			F->elsePending = 0;
			--F->indent;
			TRY(AddStatement(F, str));
			StringBuffer_prune(str);
		}

		while(RemoveFromSet(F->repeats, F->pc + 1))
		{
			StringBuffer_set(str, "repeat");
			TRY(AddStatement(F, str));
			StringBuffer_prune(str);
			++F->indent;
		}

		StringBuffer_prune(str);

		switch(o)
		{
		case OP_MOVE:
			/* Upvalue handling added to OP_CLOSURE */
		{
			char* bstr = NULL;
			if(a == b)
			{
				break;
			}
			if(CALL(b) < 2)
			{
				bstr = GetR(F, b);
			}
			else
			{
				UnsetPending(F, b);
			}
			if(error)
			{
				goto errorHandler;
			}
			/*
			 * Copy from one register to another
			 */
			TRY(Assign(F, REGISTER(a), bstr, a, PRIORITY(b), 1));
			break;
		}
		case OP_LOADK:
		{
			/*
			 * Constant. Store it in register.
			 */
			char *ctt = DecompileConstant(f, bc);
			TRY(Assign(F, REGISTER(a), ctt, a, 0, 1));
			free(ctt);
			break;
		}
		case OP_LOADBOOL:
		{
			if((F->nextBool == 0) || (c == 0))
			{
				/*
				 * assign boolean constant
				 */
				if(PENDING(a))
				{
					// some boolean constructs overwrite pending regs :(
					TRY(UnsetPending(F, a));
				}
				TRY(Assign(F, REGISTER(a), b ? "true" : "false", a, 0, 1));
			}
			else
			{
				/*
				 * assign boolean value
				 */
				char *test;
				TRY(test = OutputBoolean(F, NULL, 1));
				StringBuffer_printf(str, "%s", test);
				TRY(Assign(F, REGISTER(a), StringBuffer_getRef(str), a, 0, 0));
				free(test);
			}
			if(c)
				ignoreNext = 1;
			break;
		}
		case OP_LOADNIL:
		{
			int i;
			/*
			 * Read nil into register.
			 */
			for(i = a; i <= b; ++i)
			{
				TRY(Assign(F, REGISTER(i), "nil", i, 0, 1));
			}
			break;
		}
		case OP_VARARG: // Lua5.1 specific.
		{
			int i;
			/*
			 * Read ... into register.
			 */
			if(b == 0)
			{
				TRY(Assign(F, REGISTER(a), "...", a, 0, 1));
			}
			else
			{
				for(i = 0; i < b; ++i)
				{
					TRY(Assign(F, REGISTER(a + i), "...", a + i, 0, 1));
				}
			}
			break;
		}
		case OP_GETUPVAL:
		{
			TRY(Assign(F, REGISTER(a), UPVALUE(b), a, 0, 1));
			break;
		}
		case OP_GETGLOBAL:
		{
			/*
			 * Read global into register.
			 */
			TRY(Assign(F, REGISTER(a), GLOBAL(bc), a, 0, 1));
			break;
		}
		case OP_GETTABLE:
		{
			/*
			 * Read table entry into register.
			 */
			char *bstr, *cstr;
			TRY(cstr = RegisterOrConstant(F, c));
			TRY(bstr = GetR(F, b));
			if(bstr[0] == '{')
			{
				StringBuffer_printf(str, "(%s)", bstr);
			}
			else
			{
				StringBuffer_set(str, bstr);
			}
			MakeIndex(F, str, cstr, 0);
			TRY(Assign(F, REGISTER(a), StringBuffer_getRef(str), a, 0, 0));
			free(cstr);
			break;
		}
		case OP_SETGLOBAL:
		{
			/*
			 * Global Assignment statement.
			 */
			char *var = GLOBAL(bc);
			if(IS_TABLE(a))
			{
				TRY(PrintTable(F, a, 0));
			}
			{
				char *astr;
				TRY(astr = GetR(F, a));
				TRY(Assign(F, var, astr, -1, 0, 0));
			}
			break;
		}
		case OP_SETUPVAL:
		{
			/*
			 * Global Assignment statement.
			 */
			char *var = UPVALUE(bc);
			if(IS_TABLE(a))
			{
				TRY(CloseTable(F, a));
			}
			{
				char *astr;
				TRY(astr = GetR(F, a));
				TRY(Assign(F, var, astr, -1, 0, 0));
			}
			break;
		}
		case OP_SETTABLE:
		{
			char *bstr, *cstr;
			int settable;
			TRY(bstr = RegisterOrConstant(F, b));
			TRY(cstr = RegisterOrConstant(F, c));
			/*
			 * first try to add into a table
			 */
			TRY(settable = SetTable(F, a, bstr, cstr));
			if(!settable)
			{
				/*
				 * if failed, just output an assignment
				 */
				StringBuffer_set(str, REGISTER(a));
				MakeIndex(F, str, bstr, 0);
				TRY(Assign(F, StringBuffer_getRef(str), cstr, -1, 0, 0));
			}
			free(bstr);
			free(cstr);
			break;
		}
		case OP_NEWTABLE:
		{
			Instruction i2 = code[pc+1];
			OpCode o2 = GET_OPCODE(i2);
			Instruction i3 = code[pc+2];
			OpCode o3 = GET_OPCODE(i3);
			// if the next value is VARARG followed by a SETLIST this is probably a "{...}" construct
			if(o2 == OP_VARARG && o3 == OP_SETLIST)
			{
				TRY(Assign(F, REGISTER(a), "{...}", a, 0, 1));
				ignoreNext = 2;
			}
			else
			{
				TRY(StartTable(F, a, b, c));
			}
			break;
		}
		case OP_SELF:
		{
			/*
			 * Read table entry into register.
			 */
			char *bstr, *cstr;
			TRY(cstr = RegisterOrConstant(F, c));
			TRY(bstr = GetR(F, b));

			bstr = strdup(bstr);

			TRY(Assign(F, REGISTER(a + 1), bstr, a + 1, PRIORITY(b), 0));

			StringBuffer_set(str, bstr);
			MakeIndex(F, str, cstr, 1);
			TRY(Assign(F, REGISTER(a), StringBuffer_getRef(str), a, 0, 0));
			free(bstr);
			free(cstr);
			break;
		}
		case OP_ADD:
		case OP_SUB:
		case OP_MUL:
		case OP_DIV:
		case OP_POW:
		case OP_MOD:
		{
			char *bstr, *cstr;
			char *oper = operators[o];
			int prio = priorities[o];
			int bprio = PRIORITY(b);
			int cprio = PRIORITY(c);
			TRY(bstr = RegisterOrConstant(F, b));
			TRY(cstr = RegisterOrConstant(F, c));
			// FIXME: might need to change from <= to < here
			if((prio != 1 && bprio <= prio) || (prio == 1 && bstr[0] != '-'))
			{
				StringBuffer_add(str, bstr);
			}
			else
			{
				StringBuffer_addPrintf(str, "(%s)", bstr);
			}
			StringBuffer_addPrintf(str, " %s ", oper);
			// FIXME: being conservative in the use of parentheses
			if(cprio < prio)
			{
				StringBuffer_add(str, cstr);
			}
			else
			{
				StringBuffer_addPrintf(str, "(%s)", cstr);
			}
			TRY(Assign(F, REGISTER(a), StringBuffer_getRef(str), a, prio, 0));
			free(bstr);
			free(cstr);
			break;
		}
		case OP_UNM:
		case OP_NOT:
		case OP_LEN:
		{
			char *bstr;
			int prio = priorities[o];
			int bprio = PRIORITY(b);
			TRY(bstr = GetR(F, b));
			StringBuffer_add(str, operators[o]);
			if(bprio <= prio)
			{
				StringBuffer_add(str, bstr);
			}
			else
			{
				StringBuffer_addPrintf(str, "(%s)", bstr);
			}
			TRY(Assign(F, REGISTER(a), StringBuffer_getRef(str), a, 0, 0));
			break;
		}
		case OP_CONCAT:
		{
			int i;
			for(i = b; i <= c; ++i)
			{
				char *istr;
				TRY(istr = GetR(F, i));
				if(PRIORITY(i) > priorities[o])
				{
					StringBuffer_addPrintf(str, "(%s)", istr);
				}
				else
				{
					StringBuffer_add(str, istr);
				}
				if(i < c)
				{
					StringBuffer_add(str, " .. ");
				}
			}
			TRY(Assign(F, REGISTER(a), StringBuffer_getRef(str), a, 0, 0));
			break;
		}
		case OP_JMP:
		{
			int dest = sbc + pc + 2;
			Instruction idest = code[dest - 1];
			if(boolpending)
			{
				boolpending = 0;
				F->bools[F->nextBool]->dest = dest;
				++F->nextBool;
				F->bools[F->nextBool] = calloc(sizeof(BoolOp), 1);
				if(F->testpending)
				{
					F->testjump = dest;
				}
				if(RemoveFromSet(F->untils, F->pc))
				{
					int endif, thenaddr;
					char* test;
					LogicExp* exp;
					TRY(exp = MakeBoolean(F, &endif, &thenaddr));
					TRY(test = WriteBoolean(exp, &thenaddr, &endif, 0));
					StringBuffer_printf(str, "until %s", test);
					--F->indent;
					RawAddStatement(F, str);
					free(test);
				}
			}
			else if(GET_OPCODE(idest) == OP_TFORLOOP)
			{
				/*
				* generic 'for'
				*/
				int i;
				char* vname[40];

				a = GETARG_A(idest);
				c = GETARG_C(idest);

				++F->intspos;
				char *generator = GetR(F, a);
				char *control = GetR(F, a + 2);
				char *state = GetR(F, a + 1);
				for(i = 1; i <= c; ++i)
				{
					if(!IS_VARIABLE(a + 2 + i))
					{
						int i2;
						int loopvars = 0;
						vname[i-1] = NULL;
						for(i2 = 0; i2 < f->sizelocvars; ++i2)
						{
							if(f->locvars[i2].startpc == pc + 1)
							{
								++loopvars;
								//search for the loop variable. Set it's endpc one step further so it will be the same for all loop variables
								if(GET_OPCODE(F->f->code[f->locvars[i2].endpc - 2]) == OP_TFORLOOP)
								{
									f->locvars[i2].endpc -= 2;
								}
								if(GET_OPCODE(F->f->code[f->locvars[i2].endpc - 1]) == OP_TFORLOOP)
								{
									f->locvars[i2].endpc -= 1;
								}
								if(loopvars == 3 + i)
								{
									vname[i-1] = LOCAL(i2);
									break;
								}
							}
						}
						if(vname[i-1] == NULL)
						{
							vname[i-1] = malloc(5);
							sprintf(vname[i-1], "i_%d", i);
							TRY(DeclareVariable(F, vname[i-1], a + 2 + i));
						}
					}
					else
					{
						vname[i-1] = strdup(F->R[a+2+i]);
					}
					F->internal[a+2+i] = 1;
				}

				DeclarePendingLocals(F);

				StringBuffer_printf(str, "for %s", vname[0]);
				for(i = 2; i <= c; ++i)
				{
					StringBuffer_addPrintf(str, ",%s", vname[i-1]);
				}
				StringBuffer_addPrintf(str, " in ");
				StringBuffer_addPrintf(str, "%s do", generator);

				F->internal[a] = 1;
				F->internal[a + 1] = 1;
				F->internal[a + 2] = 1;

				F->intbegin[F->intspos] = a;
				F->intend[F->intspos] = a + 2 + c;
				TRY(AddStatement(F, str));
				++F->indent;
				break;
			}
			else if(GetEndifAddr(F, pc + 2))
			{
				if(F->elseWritten)
				{
					--F->indent;
					StringBuffer_printf(str, "end");
					TRY(AddStatement(F, str));
				}
				--F->indent;
				F->elsePending = dest;
				F->elseStart = pc + 2;
			}
			else if(PeekSet(F->whiles, pc))
			{
				StringBuffer_printf(str, "while 1 do");
				TRY(AddStatement(F, str));
				MarkBackpatch(F);
				++F->indent;
			}
			else if(RemoveFromSet(F->whiles, dest - 2))
			{
				--F->indent;
				StringBuffer_printf(str, "end");
				TRY(AddStatement(F, str));
				/* end while 1 */
			}
			else if(sbc == 2 && GET_OPCODE(code[pc+2]) == OP_LOADBOOL)
			{
				int boola = GETARG_A(code[pc+1]);
				char* test;
				/* skip */
				char* ra = strdup(REGISTER(boola));
				char* rb = strdup(ra);
				F->bools[F->nextBool]->op1 = ra;
				F->bools[F->nextBool]->op2 = rb;
				F->bools[F->nextBool]->op = OP_TESTSET;
				F->bools[F->nextBool]->neg = c;
				F->bools[F->nextBool]->pc = pc + 3;
				F->testpending = a + 1;
				F->bools[F->nextBool]->dest = dest;
				++F->nextBool;
				F->bools[F->nextBool] = calloc(sizeof(BoolOp), 1);
				F->testjump = dest;
				TRY(test = OutputBoolean(F, NULL, 1));
				StringBuffer_printf(str, "%s", test);
				TRY(UnsetPending(F, boola));
				TRY(Assign(F, REGISTER(boola), StringBuffer_getRef(str), boola, 0, 0));
				ignoreNext = 2;
			}
			else if(GET_OPCODE(idest) == OP_LOADBOOL)
			{
				/*
				 * constant boolean value
				 */
				pc = dest - 2;
			}
			else if(sbc == 0)
			{
				/* dummy jump -- ignore it */
				break;
			}
			else
			{
				int nextpc = pc + 1;
				int nextsbc = sbc - 1;
				for(;;)
				{
					Instruction nextins = code[nextpc];
					if(GET_OPCODE(nextins) == OP_JMP && GETARG_sBx(nextins) == nextsbc)
					{
						++nextpc;
						--nextsbc;
					}
					else
						break;
					if(nextsbc == -1)
					{
						break;
					}
				}
				if(nextsbc == -1)
				{
					pc = nextpc - 1;
					break;
				}
				if(F->indent > baseIndent)
				{
					StringBuffer_printf(str, "do return end");
				}
				else
				{
					pc = dest - 2;
				}
				TRY(AddStatement(F, str));
			}

			break;
		}
		case OP_EQ:
		case OP_LT:
		case OP_LE:
		{
			if(IS_CONSTANT(b))
			{
				int swap = b;
				b = c;
				c = swap;
				a = !a;
				if(o == OP_LT) o = OP_LE;
				else if(o == OP_LE) o = OP_LT;
			}
			TRY(F->bools[F->nextBool]->op1 = RegisterOrConstant(F, b));
			TRY(F->bools[F->nextBool]->op2 = RegisterOrConstant(F, c));
			F->bools[F->nextBool]->op = o;
			F->bools[F->nextBool]->neg = a;
			F->bools[F->nextBool]->pc = pc + 1;
			boolpending = 1;
			break;
		}
		case OP_TESTSET: // Lua5.1 specific TODO: correct it
		case OP_TEST:
		{
			int cmpa, cmpb, cmpc;
			char *ra, *rb;

			if(o == OP_TESTSET)
			{
				cmpa = a;
				cmpb = b;
				cmpc = c;
			}
			else
			{
				cmpa = a;
				cmpb = a;
				cmpc = c;
				// StringBuffer_add(str, "  -- Lua5.1 code: CHECK");
				// TRY(AddStatement(F, str));
			}

			if(!IS_VARIABLE(cmpa))
			{
				ra = strdup(REGISTER(cmpa));
				TRY(rb = GetR(F, cmpb));
				rb = strdup(rb);
				PENDING(cmpa) = 0;
			}
			else
			{
				TRY(ra = GetR(F, cmpa));
				if(cmpa != cmpb)
				{
					TRY(rb = GetR(F, cmpb));
					rb = strdup(rb);
				}
				else
				{
					rb = strdup(ra);
				}
			}
			F->bools[F->nextBool]->op1 = ra;
			F->bools[F->nextBool]->op2 = rb;
			F->bools[F->nextBool]->op = o;
			F->bools[F->nextBool]->neg = cmpc;
			F->bools[F->nextBool]->pc = pc + 1;
			// Within an IF, a and b are the same, avoiding side-effects
			if(cmpa != cmpb || !IS_VARIABLE(cmpa))
			{
				F->testpending = cmpa + 1;
			}
			boolpending = 1;
			break;
		}
		case OP_CALL:
		case OP_TAILCALL:
		{
			/*
			 * Function call. The CALL opcode works like this:
			 * R(A),...,R(A+F-2) := R(A)(R(A+1),...,R(A+B-1))
			 */
			int i, limit, self;
			char* astr;
			self = 0;

			if(b == 0)
			{

				limit = a + 1;
				while(PENDING(limit) || IS_VARIABLE(limit))
				{
					++limit;
				}
			}
			else
			{
				limit = a + b;
			}
			if(o == OP_TAILCALL)
			{
				StringBuffer_set(str, "return ");
				ignoreNext = 1;
			}
			TRY(astr = GetR(F, a));
			StringBuffer_addPrintf(str, "%s(", astr);

			{
				char* at = astr + strlen(astr) - 1;
				while(at > astr && (isalpha(*at) || *at == '_'))
				{
					--at;
				}
				if(*at == ':')
				{
					self = 1;
				}
			}

			for(i = a + 1; i < limit; ++i)
			{
				char *ireg;
				TRY(ireg = GetR(F, i));
				if(self && i == a + 1)
				{
					continue;
				}
				if(i > a + 1 + self)
				{
					StringBuffer_add(str, ", ");
				}
				if(ireg)
				{
					StringBuffer_add(str, ireg);
				}
			}
			StringBuffer_addChar(str, ')');

			if(c == 0)
			{
				F->lastCall = a;
			}
			if(GET_OPCODE(code[pc+1]) == OP_LOADNIL && GETARG_A(code[pc+1]) == a + 1)
			{
				StringBuffer_prepend(str, "(");
				StringBuffer_add(str, ")");
				c += GETARG_B(code[pc+1]) - GETARG_A(code[pc+1]) + 1;
				// ignoreNext = 1;
			}
			if(o == OP_TAILCALL || c == 1)
			{
				TRY(AddStatement(F, str));
			}
			else
			{
				TRY(Assign(F, REGISTER(a), StringBuffer_getRef(str), a, 0, 0));
				for(i = 0; i < c - 1; ++i)
				{
					CALL(a + i) = i + 1;
				}
			}
			break;
		}
		case OP_RETURN:
		{
			/*
			 * Return call. The RETURN opcode works like this: return
			 * R(A),...,R(A+B-2)
			 */
			int i, limit;

			/* skip the last RETURN */
			if(pc == n - 1)
				break;
			if(b == 0)
			{
				limit = a;
				while(PENDING(limit) || IS_VARIABLE(limit))
				{
					++limit;
				}
			}
			else
			{
				limit = a + b - 1;
			}
			StringBuffer_set(str, "return ");
			for(i = a; i < limit; ++i)
			{
				char* istr;
				if(i > a)
				{
					StringBuffer_add(str, ", ");
				}
				istr = GetR(F, i);
				TRY(StringBuffer_add(str, istr));
			}
			TRY(AddStatement(F, str));
			break;
		}
		case OP_FORLOOP: //Lua5.1 specific. TODO: CHECK
		{
			int i;

			for(i = F->intbegin[F->intspos]; i <= F->intend[F->intspos]; ++i)
			{
				//fprintf(stderr,"X %d\n",i);
				IS_VARIABLE(i) = 0;
				F->internal[i] = 0;
			}
			--F->intspos;
			--F->indent;
			F->ignore_for_variables = 0;

			StringBuffer_set(str, "end");
			TRY(AddStatement(F, str));
			break;
		}
		case OP_TFORLOOP: //Lua5.1 specific. TODO: CHECK
		{
			int i;
			for(i = F->intbegin[F->intspos]; i <= F->intend[F->intspos]; ++i)
			{
				IS_VARIABLE(i) = 0;
				F->internal[i] = 0;
			}
			--F->intspos;

			--F->indent;
			F->ignore_for_variables = 0;
			StringBuffer_set(str, "end");
			TRY(AddStatement(F, str));
			ignoreNext = 1;
			break;
		}
		case OP_FORPREP: //Lua5.1 specific. TODO: CHECK
		{
			/*
			* numeric 'for'
			*/
			int i;
			int step;
			char *idxname;
			char *initial;
			char *a1str;
			char *endstr;
			++F->intspos;
			TRY(initial = GetR(F, a));
			TRY(endstr = GetR(F, a + 2));
			TRY(a1str = GetR(F, a + 1));

			if(!IS_VARIABLE(a + 3))
			{
				int loopvars = 0;
				idxname = NULL;
				for(i = 0; i < f->sizelocvars; ++i)
				{
					if(f->locvars[i].startpc == pc + 1)
					{
						++loopvars;
						//search for the loop variable. Set it's endpc one step further so it will be the same for all loop variables
						if(GET_OPCODE(F->f->code[f->locvars[i].endpc - 1]) == OP_FORLOOP)
						{
							f->locvars[i].endpc -= 1;
						}
						if(loopvars == 4)
						{
							idxname = LOCAL(i);
							break;
						}
					}
				}
				if(idxname == NULL)
				{
					idxname = malloc(2);
					sprintf(idxname, "i");
					TRY(DeclareVariable(F, idxname, a + 3));
				}
			}
			else
			{
				idxname = strdup(F->R[a+3]);
			}
			DeclarePendingLocals(F);
			/*
			 * if A argument for FORLOOP is not a known variable,
			 * it was declared in the 'for' statement. Look for
			 * its name in the locals table.
			 */



			initial = strdup(initial);
			step = atoi(REGISTER(a + 2));

			if(step == 1)
			{
				StringBuffer_printf(str, "for %s = %s, %s do", idxname, initial, a1str);
			}
			else
			{
				StringBuffer_printf(str, "for %s = %s, %s, %s do",
									idxname, initial,
									a1str, REGISTER(a + 2));
			}

			/*
			 * Every numeric 'for' declares 4 variables.
			 */
			F->internal[a] = 1;
			F->internal[a + 1] = 1;
			F->internal[a + 2] = 1;
			F->internal[a + 3] = 1;
			F->intbegin[F->intspos] = a;
			F->intend[F->intspos] = a + 3;
			TRY(AddStatement(F, str));
			++F->indent;
			break;
		}
		case OP_SETLIST:
		{
			TRY(SetList(F, a, b, c));
			break;
		}
		case OP_CLOSE:
			/*
			 * Handled in do_opens/do_closes variables.
			 */
			break;
		case OP_CLOSURE:
		{
			/*
			 * Function.
			 */
			int i;
			int uvn;
			int cfnum = functionnum;

			uvn = f->p[c]->nups;

			/* determining upvalues */

			// upvalue names = next n opcodes after CLOSURE

			if(!f->p[c]->upvalues)
			{
				f->p[c]->sizeupvalues = uvn;
				f->p[c]->upvalues = malloc(uvn * sizeof(TString*));

				for(i = 0; i < uvn; ++i)
				{
					if(GET_OPCODE(code[pc+i+1]) == OP_MOVE)
					{
						char names[10];
						sprintf(names, "l_%d_%d", functionnum, GETARG_B(code[pc+i+1]));
						f->p[c]->upvalues[i] = luaS_new(glstate, names);
					}
					else if(GET_OPCODE(code[pc+i+1]) == OP_GETUPVAL)
					{
						f->p[c]->upvalues[i] = f->upvalues[GETARG_B(code[pc+i+1])];
					}
					else
					{
						char names[20];
						sprintf(names, "upval_%d_%d", functionnum, i);
						f->p[c]->upvalues[i] = luaS_new(glstate, names);
					}
				}
			}

			/* upvalue determinition end */

			StringBuffer_set(str, "function");
			functionnum = c + 1;
			StringBuffer_add(str, ProcessCode(f->p[c], F->indent)); // function contents
			functionnum = cfnum;
			for(i = 0; i < F->indent; ++i)
			{
				StringBuffer_add(str, "   ");
			}
			StringBuffer_add(str, "end"); // end label for function
			if(F->indent == 0)
			{
				StringBuffer_add(str, "\n");
			}
			TRY(Assign(F, REGISTER(a), StringBuffer_getRef(str), a, 0, 0));
			/* need to add upvalue handling */

			ignoreNext = f->p[c]->sizeupvalues;

			break;
		}
		default:
		{
			StringBuffer_printf(str, "-- unhandled opcode? : %-9s\t\n", luaP_opnames[o]);
			TRY(AddStatement(F, str));
			break;
		}
		}

		if(debug)
		{
			TRY(ShowState(F));
			{
				char* f = PrintFunction(F);
				fprintf(stddebug, "%s\n", f);
				free(f);
			}
		}

		if(GetEndifAddr(F, pc))
		{
			StringBuffer_set(str, "end");
			F->elseWritten = 0;
			--F->indent;
			TRY(AddStatement(F, str));
			StringBuffer_prune(str);
		}

		TRY(OutputAssignments(F));
	}

	if(GetEndifAddr(F, pc + 1))
	{
		StringBuffer_set(str, "end");
		--F->indent;
		TRY(AddStatement(F, str));
		StringBuffer_prune(str);
	}

	TRY(FlushBoolean(F));

	if(SET_CTR(F->tpend) > 0)
	{
		StringBuffer_set(str, " -- Warning: undefined locals caused missing assignments!");
		TRY(AddStatement(F, str));
	}

	while(F->indent > indent + 1)
	{
		StringBuffer_set(str, " -- Warning: missing end command somewhere! Added here.");
		TRY(AddStatement(F, str));
		--F->indent;
		StringBuffer_set(str, "end");
		TRY(AddStatement(F, str));
	}

	output = PrintFunction(F);

	DeleteFunction(F);

	return output;

errorHandler:
	printf("ERRORHANDLER\n");
	{
		char *copy;
		Statement *stmt;
		StringBuffer_printf(str, "--[[ DECOMPILER ERROR %d: %s ]]", errorCode, error);
		copy = StringBuffer_getCopy(str);
		stmt = NewStatement(copy, F->pc, F->indent);
		AddToList(&(F->statements), (ListItem *) stmt);
		F->lastLine = F->pc;
	}
	output = PrintFunction(F);
	DeleteFunction(F);
	error = NULL;
	return output;
}

void luaU_decompile(const Proto * f, int dflag)
{
	char* code;
	debug = dflag;
	functionnum = 0;
	code = ProcessCode(f, 0);
	printf("%s\n", code);
	free(code);
}

void luaU_decompileFunctions(const Proto* f, int dflag, int functions)
{
	int i, c;
	char* code;

	int uvn;

	c = functions - 1;
	uvn = f->p[c]->nups;

	/* determining upvalues */

	// upvalue names = next n opcodes after CLOSURE

	if(!f->p[c]->upvalues)
	{
		f->p[c]->sizeupvalues = uvn;
		f->p[c]->upvalues = malloc(uvn * sizeof(TString*));

		for(i = 0; i < uvn; ++i)
		{
			char names[10];
			sprintf(names, "l_%d_%d", 0, i);
			f->p[c]->upvalues[i] = luaS_new(glstate, names);
			printf("local l_%d_%d = nil\n", 0, i);
		}
	}

	i = functions - 1;
	debug = dflag;


	printf("DecompiledFunction = function");
	functionnum = i + 1;
	code = ProcessCode(f->p[i], 0);
	printf("%send\n", code);
	free(code);
}

#define CC(r) (IS_CONSTANT((r)) ? 'K' : 'R')
#define CV(r) (!IS_CONSTANT((r)) ? r : (r-256))
#define MAXCONSTSIZE 1024

void luaU_disassemble(const Proto* fwork, int dflag, int functions, char* name)
{
	char tmp[MAXCONSTSIZE+128];
	char tmp2[MAXCONSTSIZE+128];
	Proto* f = (Proto*)fwork; // cast avoids warning about initialization discarding 'const'
	int pc, l;
	if(functions != 0)
	{
		f = fwork->p[functions-1];
	}

	printf("; Name:            %s\n", "");
	printf("; Defined at line: %d\n", f->linedefined);
	printf("; #Upvalues:       %d\n", f->nups);
	printf("; #Parameters:     %d\n", f->numparams);
	printf("; Is_vararg:       %d\n", f->is_vararg);
	printf("; Max Stack Size:  %d\n", f->maxstacksize);
	printf("\n");

	for(pc = 0; pc < f->sizecode; ++pc)
	{
		Instruction i = f->code[pc];
		OpCode o = GET_OPCODE(i);
		int a = GETARG_A(i);
		int b = GETARG_B(i);
		int c = GETARG_C(i);
		int bc = GETARG_Bx(i);
		int sbc = GETARG_sBx(i);
		char line[100] = {'\0'};
		char lend[MAXCONSTSIZE + 128] = {'\0'};
		switch(o)
		{
		case OP_MOVE:
		{
			sprintf(line, "%c%d %c%d", CC(a), CV(a), CC(b), CV(b));
			sprintf(lend, "%c%d := %c%d", CC(a), CV(a), CC(b), CV(b));
			break;
		}
		case OP_LOADK:
		{
			sprintf(line, "%c%d K%d", CC(a), CV(a), bc);
			char *constant = DecompileConstant(f, bc);
			sprintf(lend, "%c%d := %s", CC(a), CV(a), constant);
			free(constant);
			break;
		}
		case OP_LOADBOOL:
		{
			sprintf(line, "%c%d %d %d", CC(a), CV(a), b, c);
			if(b)
			{
				if(c)
				{
					sprintf(lend, "%c%d := %s", CC(a), CV(a), "true; PC := %d", pc + 2);
				}
				else
				{
					sprintf(lend, "%c%d := %s", CC(a), CV(a), "true");
				}
			}
			else
			{
				if(c)
				{
					sprintf(lend, "%c%d := %s", CC(a), CV(a), "false; PC := %d", pc + 2);
				}
				else
				{
					sprintf(lend, "%c%d := %s", CC(a), CV(a), "false");
				}
			}
			break;
		}
		case OP_LOADNIL:
		{
			sprintf(line, "%c%d %c%d", CC(a), CV(a), CC(b), CV(b));
			lend[0] = '\0';
			for(l = a; l <= b; ++l)
			{
				sprintf(tmp, "R%d := ", l);
				strcat(lend, tmp);
			}
			strcat(lend, "nil");
			break;
		}
		case OP_VARARG:
			if(b == 0)
			{
				sprintf(line, "%c%d 0", CC(a), CV(a));
			}
			else
			{
				sprintf(line, "%c%d %c%d", CC(a), CV(a), CC(b), CV(b));
			}
			lend[0] = '\0';
			if(b == 0)
			{
				sprintf(tmp, "R%d := ", a);
				strcat(lend, tmp);
			}
			else
			{
				for(l = a; l <= b; ++l)
				{
					sprintf(tmp, "R%d := ", l);
					strcat(lend, tmp);
				}
			}
			strcat(lend, "...");
			break;
		case OP_GETUPVAL:
		{
			sprintf(line, "%c%d U%d", CC(a), CV(a), b);
			sprintf(lend, "%c%d := U%d", CC(a), CV(a), b);
			break;
		}
		case OP_GETGLOBAL:
		{
			sprintf(line, "%c%d K%d", CC(a), CV(a), bc);
			sprintf(lend, "%c%d := %s", CC(a), CV(a), GLOBAL(bc));
			break;
		}
		case OP_GETTABLE:
		{
			sprintf(line, "%c%d %c%d %c%d", CC(a), CV(a), CC(b), CV(b), CC(c), CV(c));
			if(IS_CONSTANT(c))
			{
				char *constant = DecompileConstant(f, c - 256);
				sprintf(lend, "R%d := R%d[%s]", a, b, constant);
				free(constant);
			}
			else
			{
				sprintf(lend, "R%d := R%d[R%d]", a, b, c);
			}
			break;
		}
		case OP_SETGLOBAL:
		{
			sprintf(line, "%c%d K%d", CC(a), CV(a), bc);
			sprintf(lend, "%s := %c%d", GLOBAL(bc), CC(a), CV(a));
			break;
		}
		case OP_SETUPVAL:
		{
			sprintf(line, "%c%d U%d", CC(a), CV(a), b);
			sprintf(lend, "U%d := %cd", b, CC(a), CV(a));
			break;
		}
		case OP_SETTABLE:
		{
			sprintf(line, "%c%d %c%d %c%d", CC(a), CV(a), CC(b), CV(b), CC(c), CV(c));
			if(IS_CONSTANT(b))
			{
				char *constantb = DecompileConstant(f, b - 256);
				if(IS_CONSTANT(c))
				{
					char *constantc = DecompileConstant(f, c - 256);
					sprintf(lend, "R%d[%s] := %s", a, constantb, constantc);
					free(constantc);
				}
				else
				{
					sprintf(lend, "R%d[%s] := R%d", a, constantb, c);
				}
				free(constantb);
			}
			else
			{
				if(IS_CONSTANT(c))
				{
					char *constantc = DecompileConstant(f, c - 256);
					sprintf(lend, "R%d[R%d] := %s", a, b, constantc);
					free(constantc);
				}
				else
				{
					sprintf(lend, "R%d[R%d] := R%d", a, b, c);
				}
			}
			break;
		}
		case OP_NEWTABLE:
			sprintf(line, "%c%d %d %d", CC(a), CV(a), b, c);
			sprintf(lend, "%c%d := {}", CC(a), CV(a));
			break;
		case OP_SELF:
		{
			sprintf(line, "R%d R%d %c%d", a, b, CC(c), CV(c));
			if(IS_CONSTANT(c))
			{
				char *constant = DecompileConstant(f, c - 256);
				sprintf(lend, "R%d := R%d; R%d := R%d[%s]", a + 1, b, a, b, constant);
				free(constant);
			}
			else
			{
				sprintf(lend, "R%d := R%d; R%d := R%d[R%d]", a + 1, b, a, b, c);
			}
			break;
		}
		case OP_ADD:
		case OP_SUB:
		case OP_MUL:
		case OP_DIV:
		case OP_POW:
		case OP_MOD:
			sprintf(line, "%c%d %c%d %c%d", CC(a), CV(a), CC(b), CV(b), CC(c), CV(c));
			if(IS_CONSTANT(b))
			{
				char *constantb = DecompileConstant(f, b - 256);
				if(IS_CONSTANT(c))
				{
					char *constantc = DecompileConstant(f, c - 256);
					sprintf(lend, "R%d := %s %s %s", a, constantb, operators[o], constantc);
					free(constantc);
				}
				else
				{
					sprintf(lend, "R%d := %s %s R%d", a, constantb, operators[o], c);
				}
				free(constantb);
			}
			else
			{
				if(IS_CONSTANT(c))
				{
					char *constantc = DecompileConstant(f, c - 256);
					sprintf(lend, "R%d := R%d %s %s", a, b, operators[o], constantc);
					free(constantc);
				}
				else
				{
					sprintf(lend, "R%d := R%d %s R%d", a, b, operators[o], c);
				}
			}
			break;
		case OP_UNM:
		case OP_NOT:
		case OP_LEN:
		{
			sprintf(line, "%c%d %c%d", CC(a), CV(a), CC(b), CV(b));
			if(IS_CONSTANT(b))
			{
				char *constant = DecompileConstant(f, b - 256);
				sprintf(lend, "R%d := %s %s", a, operators[o], constant);
				free(constant);
			}
			else
			{
				sprintf(lend, "R%d := %s R%d", a, operators[o], b);
			}
			break;
		}
		case OP_CONCAT:
			sprintf(line, "%c%d %c%d %c%d", CC(a), CV(a), CC(b), CV(b), CC(c), CV(c));
			sprintf(lend, "R%d := ", a);
			for(l = b; l < c; ++l)
			{
				sprintf(tmp, "R%d .. ", l);
				strcat(lend, tmp);
			}
			sprintf(tmp, "R%d", c);
			strcat(lend, tmp);
			break;
		case OP_JMP:
		{
			int dest = sbc + pc + 2;
			sprintf(line, "%d", dest);
			sprintf(lend, "PC := %d", dest);
		}
		break;
		case OP_EQ:
		case OP_LT:
		case OP_LE:
		{
			int dest = GETARG_sBx(f->code[pc+1]) + pc + 1 + 2;
			sprintf(line, "%d %c%d %c%d", a, CC(b), CV(b), CC(c), CV(c));
			sprintf(tmp, "R%d", b);
			sprintf(tmp2, "R%d", c);
			if(IS_CONSTANT(b))
			{
				char *constant = DecompileConstant(f, b - 256);
				sprintf(tmp, "%s", constant);
				free(constant);
			}
			if(IS_CONSTANT(c))
			{
				char *constant = DecompileConstant(f, c - 256);
				sprintf(tmp2, "%s", constant);
				free(constant);
			}
			if(a)
			{
				sprintf(lend, "if %s %s %s then PC := %d", tmp, opstr(o), tmp2, dest);
			}
			else
			{
				sprintf(lend, "if %s %s %s then PC := %d", tmp, invopstr(o), tmp2, dest);
			}
			break;
		}
		case OP_TEST:
		{
			int dest = GETARG_sBx(f->code[pc+1]) + pc + 1 + 2;
			sprintf(line, "%c%d %d", CC(a), CV(a), c);
			sprintf(tmp, "R%d", a);
			if(IS_CONSTANT(a))
			{
				char *constant = DecompileConstant(f, a - 256);
				sprintf(tmp, "%s", constant);
				free(constant);
			}
			if(c)
			{
				sprintf(lend, "if %s then PC := %d", tmp, dest);
			}
			else
			{
				sprintf(lend, "if not %s then PC := %d", tmp, dest);
			}
			break;
		}
		case OP_TESTSET:
		{
			int dest = GETARG_sBx(f->code[pc+1]) + pc + 1 + 2;
			sprintf(line, "%c%d %c%d %d", CC(a), CV(a), CC(b), CV(b), c);
			sprintf(tmp, "R%d", a);
			sprintf(tmp2, "R%d", b);
			if(IS_CONSTANT(a))
			{
				char *constant = DecompileConstant(f, a - 256);
				sprintf(tmp, "%s", constant);
				free(constant);
			}
			if(IS_CONSTANT(b))
			{
				char *constant = DecompileConstant(f, b - 256);
				sprintf(tmp2, "%s", constant);
				free(constant);
			}
			if(c)
			{
				sprintf(lend, "if %s then PC := %d else %s := %s", tmp2, dest, tmp, tmp2);
			}
			else
			{
				sprintf(lend, "if not %s then PC := %d else %s := %s", tmp2, dest, tmp, tmp2);
			}
			break;
		}
		case OP_CALL:
		case OP_TAILCALL:
		{
			sprintf(line, "R%d %d %d", a, b, c);
			if(b >= 2)
			{
				tmp[0] = '\0';
				for(l = a + 1; l < a + b - 1; ++l)
				{
					sprintf(lend, "R%d,", l);
					strcat(tmp, lend);
				}
				sprintf(lend, "R%d", a + b - 1);
				strcat(tmp, lend);
			}
			else if(b == 0)
			{
				sprintf(tmp, "R%d,...", a + 1);
			}
			else
			{
				tmp[0] = '\0';
			}

			if(c >= 2)
			{
				tmp2[0] = '\0';
				for(l = a; l < a + c - 2; ++l)
				{
					sprintf(lend, "R%d,", l);
					strcat(tmp2, lend);
				}
				sprintf(lend, "R%d := ", a + c - 2);
				strcat(tmp2, lend);
			}
			else if(c == 0)
			{
				sprintf(tmp2, "R%d,... := ", a);
			}
			else
			{
				tmp2[0] = '\0';
			}
			sprintf(lend, "%sR%d(%s)", tmp2, a, tmp);
		}
		break;
		case OP_RETURN:
		{
			sprintf(line, "R%d %d", a, b);
			if(b >= 2)
			{
				tmp[0] = '\0';
				for(l = a; l < a + b - 2; ++l)
				{
					sprintf(lend, "R%d,", l);
					strcat(tmp, lend);
				}
				sprintf(lend, "R%d", a + b - 2);
				strcat(tmp, lend);
			}
			else if(b == 0)
			{
				sprintf(tmp, "R%d,...", a);
			}
			else
			{
				tmp[0] = '\0';
			}
			sprintf(lend, "return %s", tmp);
			break;
		}
		case OP_FORLOOP:
		{
			sprintf(line, "R%d %d", a, pc + sbc + 2);
			sprintf(lend, "R%d += R%d; if R%d <= R%d then begin PC := %d; R%d := R%d end", a, a + 2, a, a + 1, pc + sbc + 2, a + 3, a);
			break;
		}
		case OP_TFORLOOP:
		{
			int dest = GETARG_sBx(f->code[pc+1]) + pc + 1 + 2;
			sprintf(line, "R%d %d", a, c);
			if(c >= 1)
			{
				tmp2[0] = '\0';
				for(l = a + 3; l < a + c + 2; ++l)
				{
					sprintf(lend, "R%d,", l);
					strcat(tmp2, lend);
				}
				sprintf(lend, "R%d := ", a + c + 2);
				strcat(tmp2, lend);
			}
			else
			{
				sprintf(tmp2, "R%d,... := ", a);
			}
			sprintf(lend, "%s R%d(R%d,R%d); if R%d ~= nil then begin PC = %d; R%d := R%d end", tmp2, a, a + 1, a + 2, a + 3, dest, a + 2, a + 3);
			break;
		}
		case OP_FORPREP:
		{
			sprintf(line, "R%d %d", a, pc + sbc + 2);
			sprintf(lend, "R%d -= R%d; PC := %d", a, a + 2, pc + sbc + 2);
			break;
		}
		case OP_SETLIST:
		{
			sprintf(line, "R%d %d %d", a, b, c);
			//sprintf(lend, "R%d[(%d-1)*FPF+i] := R(%d+i), 1 <= i <= %d", a, c, a, b);
			if(c == 1)
			{
				sprintf(lend, "R%d[i] := R(%d+i), 1 <= i <= %d", a, a, b);
			}
			else
			{
				sprintf(lend, "R%d[%d*FPF+i] := R(%d+i), 1 <= i <= %d", a, c - 1, a, b);
			}
			break;
		}
		case OP_CLOSE:
		{
			sprintf(line, "R%d", a);
			sprintf(lend, "SAVE R%d,...", a);
			break;
		}
		case OP_CLOSURE:
			sprintf(line, "R%d %d", a, bc);
			if(strlen(name) == 0)
			{
				sprintf(lend, "R%d := closure(Function #%d)", a, bc + 1);
			}
			else
			{
				sprintf(lend, "R%d := closure(Function #%s.%d)", a, name, bc + 1);
			}
			break;
		default:
			break;
		}
		printf("%3d [-]: %-9s %-13s; %s\n", pc + 1, luaP_opnames[o], line, lend);
	}
	printf("\n\n");
	if(f->sizep != 0)
	{
		for(pc = 0; pc < f->sizep; ++pc)
		{
			char n[256];
			if(strlen(name) == 0)
			{
				sprintf(n, "%d", pc + 1);
			}
			else
			{
				sprintf(n, "%s.%d", name, pc + 1);
			}
			printf("; Function #%s:\n", n);
			printf(";\n");
			luaU_disassemble(f->p[pc], dflag, 0, n);
		}
	}
}

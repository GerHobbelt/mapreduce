/* ----------------------------------------------------------------------
   APP
   contact info, copyright info, etc
------------------------------------------------------------------------- */

#include "mpi.h"
#include "math.h"
#include "stdlib.h"
#include "string.h"
#include "ctype.h"
#include "unistd.h"
#include "variable.h"
#include "universe.h"
#include "object.h"
#include "memory.h"
#include "error.h"

using namespace APP_NS;

#define VARDELTA 4
#define MAXLEVEL 4

#define MYROUND(a) (( a-floor(a) ) >= .5) ? ceil(a) : floor(a)

enum{INDEX,LOOP,EQUAL,WORLD,UNIVERSE,ULOOP};
enum{ARG,OP};
enum{DONE,ADD,SUBTRACT,MULTIPLY,DIVIDE,CARAT,UNARY,
       SQRT,EXP,LN,LOG,SIN,COS,TAN,ASIN,ACOS,ATAN,
       CEIL,FLOOR,ROUND,VALUE};

/* ---------------------------------------------------------------------- */

Variable::Variable(APP *app) : Pointers(app)
{
  MPI_Comm_rank(world,&me);

  nvar = maxvar = 0;
  names = NULL;
  style = NULL;
  num = NULL;
  index = NULL;
  data = NULL;

  precedence[DONE] = 0;
  precedence[ADD] = precedence[SUBTRACT] = 1;
  precedence[MULTIPLY] = precedence[DIVIDE] = 2;
  precedence[CARAT] = 3;
  precedence[UNARY] = 4;
}

/* ---------------------------------------------------------------------- */

Variable::~Variable()
{
  for (int i = 0; i < nvar; i++) {
    delete [] names[i];
    for (int j = 0; j < num[i]; j++) delete [] data[i][j];
    delete [] data[i];
  }
  memory->sfree(names);
  memory->sfree(style);
  memory->sfree(num);
  memory->sfree(index);
  memory->sfree(data);
}

/* ----------------------------------------------------------------------
   called by variable command in input script
------------------------------------------------------------------------- */

void Variable::set(int narg, char **arg)
{
  if (narg < 2) error->all("Illegal variable command");

  // DELETE
  // doesn't matter if variable no longer exists

  if (strcmp(arg[1],"delete") == 0) {
    if (narg != 2) error->all("Illegal variable command");
    if (find(arg[0]) >= 0) remove(find(arg[0]));
    return;

  // INDEX
  // num = listed args, index = 1st value, data = copied args

  } else if (strcmp(arg[1],"index") == 0) {
    if (narg < 3) error->all("Illegal variable command");
    if (find(arg[0]) >= 0) return;
    if (nvar == maxvar) extend();
    style[nvar] = INDEX;
    num[nvar] = narg - 2;
    index[nvar] = 0;
    data[nvar] = new char*[num[nvar]];
    copy(num[nvar],&arg[2],data[nvar]);

  // LOOP
  // num = N, index = 1st value, data = list of NULLS since never used

  } else if (strcmp(arg[1],"loop") == 0) {
    if (narg != 3) error->all("Illegal variable command");
    if (find(arg[0]) >= 0) return;
    if (nvar == maxvar) extend();
    style[nvar] = LOOP;
    num[nvar] = atoi(arg[2]);
    index[nvar] = 0;
    data[nvar] = new char*[num[nvar]];
    for (int i = 0; i < num[nvar]; i++) data[nvar][i] = NULL;
    
  // EQUAL
  // remove pre-existing var if also style EQUAL (allows it to be reset)
  // num = 2, index = 1st value
  // data = 2 values, 1st is string to eval, 2nd is filled on retrieval

  } else if (strcmp(arg[1],"equal") == 0) {
    if (narg != 3) error->all("Illegal variable command");
    if (find(arg[0]) >= 0) {
      if (style[find(arg[0])] != EQUAL)
	error->all("Cannot redefine variable as a different style");
      remove(find(arg[0]));
    }
    if (nvar == maxvar) extend();
    style[nvar] = EQUAL;
    num[nvar] = 2;
    index[nvar] = 0;
    data[nvar] = new char*[num[nvar]];
    copy(1,&arg[2],data[nvar]);
    data[nvar][1] = NULL;
    
  // WORLD
  // num = listed args, index = partition this proc is in, data = copied args
  // error check that num = # of worlds in universe

  } else if (strcmp(arg[1],"world") == 0) {
    if (narg < 3) error->all("Illegal variable command");
    if (find(arg[0]) >= 0) return;
    if (nvar == maxvar) extend();
    style[nvar] = WORLD;
    num[nvar] = narg - 2;
    if (num[nvar] != universe->nworlds)
      error->all("World variable count doesn't match # of partitions");
    index[nvar] = universe->iworld;
    data[nvar] = new char*[num[nvar]];
    copy(num[nvar],&arg[2],data[nvar]);

  // UNIVERSE and ULOOP
  // for UNIVERSE: num = listed args, data = copied args
  // for ULOOP: num = N, data = list of NULLS since never used
  // index = partition this proc is in
  // universe proc 0 creates lock file
  // error check that all other universe/uloop variables are same length

  } else if (strcmp(arg[1],"universe") == 0 || strcmp(arg[1],"uloop") == 0) {
    if (strcmp(arg[1],"universe") == 0) {
      if (narg < 3) error->all("Illegal variable command");
      if (find(arg[0]) >= 0) return;
      if (nvar == maxvar) extend();
      style[nvar] = UNIVERSE;
      num[nvar] = narg - 2;
      data[nvar] = new char*[num[nvar]];
      copy(num[nvar],&arg[2],data[nvar]);
    } else {
      if (narg != 3) error->all("Illegal variable command");
      if (find(arg[0]) >= 0) return;
      if (nvar == maxvar) extend();
      style[nvar] = ULOOP;
      num[nvar] = atoi(arg[2]);
      data[nvar] = new char*[num[nvar]];
      for (int i = 0; i < num[nvar]; i++) data[nvar][i] = NULL;
    }

    if (num[nvar] < universe->nworlds)
      error->all("Universe/uloop variable count < # of partitions");
    index[nvar] = universe->iworld;

    if (universe->me == 0) {
      FILE *fp = fopen("tmp.app.variable","w");
      fprintf(fp,"%d\n",universe->nworlds);
      fclose(fp);
    }

    for (int jvar = 0; jvar < nvar; jvar++)
      if (num[jvar] && (style[jvar] == UNIVERSE || style[jvar] == ULOOP) && 
	  num[nvar] != num[jvar])
	error->all("All universe/uloop variables must have same # of values");

    if (me == 0) {
      if (universe->uscreen)
	fprintf(universe->uscreen,
		"Initial ${%s} setting: value %d on partition %d\n",
		arg[0],index[nvar]+1,universe->iworld);
      if (universe->ulogfile)
	fprintf(universe->ulogfile,
		"Initial ${%s} setting: value %d on partition %d\n",
		arg[0],index[nvar]+1,universe->iworld);
    }
    
  } else error->all("Illegal variable command");

  // set name of variable
  // must come at end, since EQUAL reset may have removed name
  // name must be all alphanumeric chars or underscores

  int n = strlen(arg[0]) + 1;
  names[nvar] = new char[n];
  strcpy(names[nvar],arg[0]);

  for (int i = 0; i < n-1; i++)
    if (!isalnum(names[nvar][i]) && names[nvar][i] != '_')
      error->all("Variable name must be alphanumeric or underscore characters");

  nvar++;
}

/* ----------------------------------------------------------------------
   single-value INDEX variable created by command-line argument
------------------------------------------------------------------------- */

void Variable::set(char *name, char *value)
{
  char **newarg = new char*[3];
  newarg[0] = name;
  newarg[1] = (char *) "index";
  newarg[2] = value;
  set(3,newarg);
  delete [] newarg;
}

/* ----------------------------------------------------------------------
   increment variable(s)
   return 0 if OK if successfully incremented
   return 1 if any variable is exhausted, free the variable to allow re-use
------------------------------------------------------------------------- */

int Variable::next(int narg, char **arg)
{
  int ivar;

  if (narg == 0) error->all("Illegal next command");

  // check that variables exist and are all the same style
  // exception: UNIVERSE and ULOOP variables can be mixed in same next command

  for (int iarg = 0; iarg < narg; iarg++) {
    ivar = find(arg[iarg]);
    if (ivar == -1) error->all("Invalid variable in next command");
    if (style[ivar] == ULOOP && style[find(arg[0])] == UNIVERSE) continue;
    else if (style[ivar] == UNIVERSE && style[find(arg[0])] == ULOOP) continue;
    else if (style[ivar] != style[find(arg[0])])
      error->all("All variables in next command must be same style");
  }

  // invalid styles EQUAL or WORLD

  int istyle = style[find(arg[0])];
  if (istyle == EQUAL || istyle == WORLD)
    error->all("Invalid variable style with next command");

  // increment all variables in list
  // if any variable is exhausted, set flag = 1 and remove var to allow re-use

  int flag = 0;

  if (istyle == INDEX || istyle == LOOP) {
    for (int iarg = 0; iarg < narg; iarg++) {
      ivar = find(arg[iarg]);
      index[ivar]++;
      if (index[ivar] >= num[ivar]) {
	flag = 1;
	remove(ivar);
      }
    }

  } else if (istyle == UNIVERSE || istyle == ULOOP) {

    // wait until lock file can be created and owned by proc 0 of this world
    // read next available index and Bcast it within my world
    // set all variables in list to nextindex

    int nextindex;
    if (me == 0) {
      while (1) {
	if (!rename("tmp.app.variable","tmp.app.variable.lock")) break;
	usleep(100000);
      }
      FILE *fp = fopen("tmp.app.variable.lock","r");
      fscanf(fp,"%d",&nextindex);
      fclose(fp);
      fp = fopen("tmp.app.variable.lock","w");
      fprintf(fp,"%d\n",nextindex+1);
      fclose(fp);
      rename("tmp.app.variable.lock","tmp.app.variable");
      if (universe->uscreen)
	fprintf(universe->uscreen,
		"Increment via next: value %d on partition %d\n",
		nextindex+1,universe->iworld);
      if (universe->ulogfile)
	fprintf(universe->ulogfile,
		"Increment via next: value %d on partition %d\n",
		nextindex+1,universe->iworld);
    }
    MPI_Bcast(&nextindex,1,MPI_INT,0,world);

    for (int iarg = 0; iarg < narg; iarg++) {
      ivar = find(arg[iarg]);
      index[ivar] = nextindex;
      if (index[ivar] >= num[ivar]) {
	flag = 1;
	remove(ivar);
      }
    }
  }

  return flag;
}

/* ----------------------------------------------------------------------
   return ptr to the data text associated with a variable
   if EQUAL var, evaluates variable and puts result in str
   return NULL if no variable or index is bad, caller must respond
------------------------------------------------------------------------- */

char *Variable::retrieve(char *name)
{
  int ivar = find(name);
  if (ivar == -1) return NULL;
  if (index[ivar] >= num[ivar]) return NULL;

  char *str;
  if (style[ivar] == INDEX || style[ivar] == WORLD || 
      style[ivar] == UNIVERSE) {
    str = data[ivar][index[ivar]];
  } else if (style[ivar] == LOOP || style[ivar] == ULOOP) {
    char *value = new char[16];
    sprintf(value,"%d",index[ivar]+1);
    int n = strlen(value) + 1;
    if (data[ivar][0]) delete [] data[ivar][0];
    data[ivar][0] = new char[n];
    strcpy(data[ivar][0],value);
    delete [] value;
    str = data[ivar][0];
  } else if (style[ivar] == EQUAL) {
    char result[32];
    double answer = evaluate(data[ivar][0],NULL);
    sprintf(result,"%.15g",answer);
    int n = strlen(result) + 1;
    if (data[ivar][1]) delete [] data[ivar][1];
    data[ivar][1] = new char[n];
    strcpy(data[ivar][1],result);
    str = data[ivar][1];
  }

  return str;
}

/* ----------------------------------------------------------------------
   return result of equal-style variable evaluation
------------------------------------------------------------------------- */

double Variable::compute_equal(int ivar)
{
  return evaluate(data[ivar][0],NULL);
}

/* ----------------------------------------------------------------------
   search for name in list of variables names
   return index or -1 if not found
------------------------------------------------------------------------- */
  
int Variable::find(char *name)
{
  for (int i = 0; i < nvar; i++)
    if (strcmp(name,names[i]) == 0) return i;
  return -1;
}

/* ----------------------------------------------------------------------
   return 1 if variable is EQUAL style, 0 if not
------------------------------------------------------------------------- */
  
int Variable::equalstyle(int ivar)
{
  if (style[ivar] == EQUAL) return 1;
  return 0;
}

/* ----------------------------------------------------------------------
   remove Nth variable from list and compact list
------------------------------------------------------------------------- */
  
void Variable::remove(int n)
{
  delete [] names[n];
  for (int i = 0; i < num[n]; i++) delete [] data[n][i];
  delete [] data[n];

  for (int i = n+1; i < nvar; i++) {
    names[i-1] = names[i];
    style[i-1] = style[i];
    num[i-1] = num[i];
    index[i-1] = index[i];
    data[i-1] = data[i];
  }
  nvar--;
}

/* ----------------------------------------------------------------------
  make space in arrays for new variable
------------------------------------------------------------------------- */

void Variable::extend()
{
  maxvar += VARDELTA;
  names = (char **)
    memory->srealloc(names,maxvar*sizeof(char *),"var:names");
  style = (int *) memory->srealloc(style,maxvar*sizeof(int),"var:style");
  num = (int *) memory->srealloc(num,maxvar*sizeof(int),"var:num");
  index = (int *) memory->srealloc(index,maxvar*sizeof(int),"var:index");
  data = (char ***) 
    memory->srealloc(data,maxvar*sizeof(char **),"var:data");
}

/* ----------------------------------------------------------------------
   copy narg strings from **from to **to 
------------------------------------------------------------------------- */
  
void Variable::copy(int narg, char **from, char **to)
{
  int n;

  for (int i = 0; i < narg; i++) {
    n = strlen(from[i]) + 1;
    to[i] = new char[n];
    strcpy(to[i],from[i]);
  }
}

/* ----------------------------------------------------------------------
   recursive evaluation of a string str
   string is a equal-style or atom-style formula containing one or more items:
     number = 0.0, -5.45, 2.8e-4, ...
     math operation = (),-x,x+y,x-y,x*y,x/y,x^y,
                      sqrt(x),exp(x),ln(x),log(x),
		      sin(x),cos(x),tan(x),asin(x),acos(x),atan(x)
     variable = v_name
   if tree ptr passed in, return ptr to parse tree created for formula
   if no tree ptr passed in, return value of string as double
------------------------------------------------------------------------- */

double Variable::evaluate(char *str, Tree **tree)
{
  int op,opprevious;
  double value1,value2;
  char onechar;
  char *ptr;

  double argstack[MAXLEVEL];
  Tree *treestack[MAXLEVEL];
  int opstack[MAXLEVEL];
  int nargstack = 0;
  int ntreestack = 0;
  int nopstack = 0;

  int i = 0;
  int expect = ARG;

  while (1) {
    onechar = str[i];

    // whitespace: just skip

    if (isspace(onechar)) i++;

    // ----------------
    // parentheses: recursively evaluate contents of parens
    // ----------------

    else if (onechar == '(') {
      if (expect == OP) error->all("Invalid syntax in variable formula");
      expect = OP;

      char *contents;
      i = find_matching_paren(str,i,contents);
      i++;

      // evaluate contents and push on stack

      if (tree) {
	Tree *newtree;
	double tmp = evaluate(contents,&newtree);
	treestack[ntreestack++] = newtree;
      } else argstack[nargstack++] = evaluate(contents,NULL);

      delete [] contents;

    // ----------------
    // number: push value onto stack
    // ----------------

    } else if (isdigit(onechar) || onechar == '.') {
      if (expect == OP) error->all("Invalid syntax in variable formula");
      expect = OP;

      // istop = end of number, including scientific notation

      int istart = i;
      while (isdigit(str[i]) || str[i] == '.') i++;
      if (str[i] == 'e' || str[i] == 'E') {
	i++;
	if (str[i] == '+' || str[i] == '-') i++;
	while (isdigit(str[i])) i++;
      }
      int istop = i - 1;

      int n = istop - istart + 1;
      char *number = new char[n+1];
      strncpy(number,&str[istart],n);
      number[n] = '\0';

      if (tree) {
        Tree *newtree = new Tree();
	newtree->type = VALUE;
	newtree->value = atof(number);
	newtree->left = newtree->right = NULL;
	treestack[ntreestack++] = newtree;
      } else argstack[nargstack++] = atof(number);

      delete [] number;

    // ----------------
    // letter: v_name, exp(), object(), keyword
    // ----------------

    } else if (islower(onechar)) {
      if (expect == OP) error->all("Invalid syntax in variable formula");
      expect = OP;

      // istop = end of word
      // word = all alphanumeric or underscore

      int istart = i;
      while (isalnum(str[i]) || str[i] == '_') i++;
      int istop = i-1;

      int n = istop - istart + 1;
      char *word = new char[n+1];
      strncpy(word,&str[istart],n);
      word[n] = '\0';

      // ----------------
      // variable
      // ----------------

      if (strncmp(word,"v_",2) == 0) {
	n = strlen(word) - 2 + 1;
	char *id = new char[n];
	strcpy(id,&word[2]);

	int ivar = find(id);
	if (ivar < 0) error->all("Invalid variable name in variable formula");

	// parse zero or one trailing brackets
	// point i beyond last bracket
	// nbracket = # of bracket pairs
	// index = int inside bracket

	int nbracket,index;
	if (str[i] != '[') nbracket = 0;
	else {
	  nbracket = 1;
	  ptr = &str[i];
	  index = int_between_brackets(ptr);
	  i = ptr-str+1;
	}

        // v_name = scalar from non atom-style global scalar

	if (nbracket == 0) {

	  char *var = retrieve(id);
	  if (var == NULL)
	    error->all("Invalid variable evaluation in variable formula");
	  if (tree) {
	    Tree *newtree = new Tree();
	    newtree->type = VALUE;
	    newtree->value = atof(var);
	    newtree->left = newtree->right = NULL;
	    treestack[ntreestack++] = newtree;
	  } else argstack[nargstack++] = atof(var);

	} else error->all("Mismatched variable in variable formula");

	delete [] id;

      // ----------------
      // math/object function or keyword
      // ----------------

      } else {

	// ----------------
	// math or object function
	// ----------------

	if (str[i] == '(') {
	  char *contents;
	  i = find_matching_paren(str,i,contents);
	  i++;

	  if (math_function(word,contents,tree,
			    treestack,ntreestack,argstack,nargstack));
	  else if (object_function(word,contents,tree,
				   treestack,ntreestack,argstack,nargstack));
	  else error->all("Invalid math or object function "
			  "in variable formula");
	  delete [] contents;

	// ----------------
	// keyword
	// ----------------

	} else {
	  int flag = keyword(word,&value1);
	  if (flag) error->all("Invalid keyword in variable formula");
	  if (tree) {
	    Tree *newtree = new Tree();
	    newtree->type = VALUE;
	    newtree->value = value1;
	    newtree->left = newtree->right = NULL;
	    treestack[ntreestack++] = newtree;
	  } else argstack[nargstack++] = value1;
	}
      }

      delete [] word;

    // ----------------
    // math operator, including end-of-string
    // ----------------

    } else if (strchr("+-*/^\0",onechar)) {
      if (onechar == '+') op = ADD;
      else if (onechar == '-') op = SUBTRACT;
      else if (onechar == '*') op = MULTIPLY;
      else if (onechar == '/') op = DIVIDE;
      else if (onechar == '^') op = CARAT;
      else op = DONE;
      i++;

      if (op == SUBTRACT && expect == ARG) {
	opstack[nopstack++] = UNARY;
	continue;
      }

      if (expect == ARG) error->all("Invalid syntax in variable formula");
      expect = ARG;

      // evaluate stack as deep as possible while respecting precedence
      // before pushing current op onto stack

      while (nopstack && precedence[opstack[nopstack-1]] >= precedence[op]) {
	opprevious = opstack[--nopstack];

	if (tree) {
	  Tree *newtree = new Tree();
	  newtree->type = opprevious;
	  if (opprevious == UNARY) {
	    newtree->left = treestack[--ntreestack];
	    newtree->right = NULL;
	  } else {
	    newtree->right = treestack[--ntreestack];
	    newtree->left = treestack[--ntreestack];
	  }
	  treestack[ntreestack++] = newtree;

	} else {
	  value2 = argstack[--nargstack];
	  if (opprevious != UNARY) value1 = argstack[--nargstack];

	  if (opprevious == ADD)
	    argstack[nargstack++] = value1 + value2;
	  else if (opprevious == SUBTRACT)
	    argstack[nargstack++] = value1 - value2;
	  else if (opprevious == MULTIPLY)
	    argstack[nargstack++] = value1 * value2;
	  else if (opprevious == DIVIDE) {
	    if (value2 == 0.0) error->all("Divide by 0 in variable formula");
	    argstack[nargstack++] = value1 / value2;
	  } else if (opprevious == CARAT) {
	    if (value2 == 0.0) error->all("Power by 0 in variable formula");
	    argstack[nargstack++] = pow(value1,value2);
	  } else if (opprevious == UNARY)
	    argstack[nargstack++] = -value2;
	}
      }

      // if end-of-string, break out of entire formula evaluation loop

      if (op == DONE) break;

      // push current operation onto stack

      opstack[nopstack++] = op;

    } else error->all("Invalid syntax in variable formula");
  }

  if (nopstack) error->all("Invalid syntax in variable formula");

  // for atom-style variable, return remaining tree
  // for equal-style variable, return remaining arg

  if (tree) {
    if (ntreestack != 1) error->all("Invalid syntax in variable formula");
    *tree = treestack[0];
    return 0.0;
  } else {
    if (nargstack != 1) error->all("Invalid syntax in variable formula");
    return argstack[0];
  }
}

/* ----------------------------------------------------------------------
   process an evaulation tree
   customize by adding a math function:
     sqrt(),exp(),ln(),log(),sin(),cos(),tan(),asin(),acos(),atan()
     ceil(),floor(),round()
---------------------------------------------------------------------- */

double Variable::eval_tree(Tree *tree, int i)
{
  if (tree->type == VALUE) return tree->value;

  if (tree->type == ADD)
    return eval_tree(tree->left,i) + eval_tree(tree->right,i);
  if (tree->type == SUBTRACT)
    return eval_tree(tree->left,i) - eval_tree(tree->right,i);
  if (tree->type == MULTIPLY)
    return eval_tree(tree->left,i) * eval_tree(tree->right,i);
  if (tree->type == DIVIDE) {
    double denom = eval_tree(tree->right,i);
    if (denom == 0.0) error->all("Divide by 0 in variable formula");
    return eval_tree(tree->left,i) / denom;
  }
  if (tree->type == CARAT) {
    double exponent = eval_tree(tree->right,i);
    if (exponent == 0.0) error->all("Power by 0 in variable formula");
    return pow(eval_tree(tree->left,i),exponent);
  }
  if (tree->type == UNARY)
    return -eval_tree(tree->left,i);

  if (tree->type == SQRT) {
    double arg = eval_tree(tree->left,i);
    if (arg < 0.0) error->all("Sqrt of negative in variable formula");
    return sqrt(arg);
  }
  if (tree->type == EXP)
    return exp(eval_tree(tree->left,i));
  if (tree->type == LN) {
    double arg = eval_tree(tree->left,i);
    if (arg <= 0.0) error->all("Log of zero/negative in variable formula");
    return log(arg);
  }
  if (tree->type == LOG) {
    double arg = eval_tree(tree->left,i);
    if (arg <= 0.0) error->all("Log of zero/negative in variable formula");
    return log10(arg);
  }

  if (tree->type == SIN)
    return sin(eval_tree(tree->left,i));
  if (tree->type == COS)
    return cos(eval_tree(tree->left,i));
  if (tree->type == TAN)
    return tan(eval_tree(tree->left,i));

  if (tree->type == ASIN) {
    double arg = eval_tree(tree->left,i);
    if (arg < -1.0 || arg > 1.0)
      error->all("Arcsin of invalid value in variable formula");
    return asin(arg);
  }
  if (tree->type == ACOS) {
    double arg = eval_tree(tree->left,i);
    if (arg < -1.0 || arg > 1.0)
      error->all("Arccos of invalid value in variable formula");
    return acos(arg);
  }
  if (tree->type == ATAN)
    return atan(eval_tree(tree->left,i));

  if (tree->type == CEIL)
    return ceil(eval_tree(tree->left,i));
  if (tree->type == FLOOR)
    return floor(eval_tree(tree->left,i));
  if (tree->type == ROUND)
    return MYROUND(eval_tree(tree->left,i));

  return 0.0;
}

/* ---------------------------------------------------------------------- */

void Variable::free_tree(Tree *tree)
{
  if (tree->left) free_tree(tree->left);
  if (tree->right) free_tree(tree->right);
  delete tree;
}

/* ----------------------------------------------------------------------
   find matching parenthesis in str, allocate contents = str between parens
   i = left paren
   return loc or right paren
------------------------------------------------------------------------- */

int Variable::find_matching_paren(char *str, int i,char *&contents)
{
  // istop = matching ')' at same level, allowing for nested parens

  int istart = i;
  int ilevel = 0;
  while (1) {
    i++;
    if (!str[i]) break;
    if (str[i] == '(') ilevel++;
    else if (str[i] == ')' && ilevel) ilevel--;
    else if (str[i] == ')') break;
  }
  if (!str[i]) error->all("Invalid syntax in variable formula");
  int istop = i;

  int n = istop - istart - 1;
  contents = new char[n+1];
  strncpy(contents,&str[istart+1],n);
  contents[n] = '\0';

  return istop;
}

/* ----------------------------------------------------------------------
   find int between brackets and return it
   ptr initially points to left bracket
   return it pointing to right bracket
   error if no right bracket or brackets are empty
   error if any between-bracket chars are non-digits or value == 0
------------------------------------------------------------------------- */

int Variable::int_between_brackets(char *&ptr)
{
  char *start = ++ptr;

  while (*ptr && *ptr != ']') {
    if (!isdigit(*ptr)) 
      error->all("Non digit character between brackets in input command");
    ptr++;
  }

  if (*ptr != ']') error->all("Mismatched brackets in input command");
  if (ptr == start) error->all("Empty brackets in input command");

  *ptr = '\0';
  int index = atoi(start);
  *ptr = ']';

  if (index == 0) 
    error->all("Index between input command brackets must be positive");
  return index;
}

/* ----------------------------------------------------------------------
   process a math function in formula
   push result onto tree or arg stack
   word = math function
   contents = str bewteen parentheses
   return 0 if not a match, 1 if successfully processed
   customize by adding a math function in 2 places:
     sqrt(),exp(),ln(),log(),sin(),cos(),tan(),asin(),acos(),atan()
     ceil(),floor(),round()
------------------------------------------------------------------------- */

int Variable::math_function(char *word, char *contents, Tree **tree,
			    Tree **treestack, int &ntreestack,
			    double *argstack, int &nargstack)
{
  // word not a match to any math function

  if (strcmp(word,"sqrt") && strcmp(word,"exp") && 
      strcmp(word,"ln") && strcmp(word,"log") &&
      strcmp(word,"sin") && strcmp(word,"cos") &&
      strcmp(word,"tan") && strcmp(word,"asin") &&
      strcmp(word,"acos") && strcmp(word,"atan") &&
      strcmp(word,"ceil") && strcmp(word,"floor") && strcmp(word,"round"))
    return 0;
    
  Tree *newtree;
  double value;

  if (tree) {
    newtree = new Tree();
    Tree *argtree;
    double tmp = evaluate(contents,&argtree);
    newtree->left = argtree;
    newtree->right = NULL;
    treestack[ntreestack++] = newtree;
  } else value = evaluate(contents,NULL);
    
  if (strcmp(word,"sqrt") == 0) {
    if (tree) newtree->type = SQRT;
    else {
      if (value < 0.0) error->all("Sqrt of negative in variable formula");
      argstack[nargstack++] = sqrt(value);
    }

  } else if (strcmp(word,"exp") == 0) {
    if (tree) newtree->type = EXP;
    else argstack[nargstack++] = exp(value);
  } else if (strcmp(word,"ln") == 0) {
    if (tree) newtree->type = LN;
    else {
      if (value <= 0.0) error->all("Log of zero/negative in variable formula");
      argstack[nargstack++] = log(value);
    }
  } else if (strcmp(word,"log") == 0) {
    if (tree) newtree->type = LOG;
    else {
      if (value <= 0.0) error->all("Log of zero/negative in variable formula");
      argstack[nargstack++] = log10(value);
    }

  } else if (strcmp(word,"sin") == 0) {
    if (tree) newtree->type = SIN;
    else argstack[nargstack++] = sin(value);
  } else if (strcmp(word,"cos") == 0) {
    if (tree) newtree->type = COS;
    else argstack[nargstack++] = cos(value);
  } else if (strcmp(word,"tan") == 0) {
    if (tree) newtree->type = TAN;
    else argstack[nargstack++] = tan(value);

  } else if (strcmp(word,"asin") == 0) {
    if (tree) newtree->type = ASIN;
    else {
      if (value < -1.0 || value > 1.0) 
	error->all("Arcsin of invalid value in variable formula");
      argstack[nargstack++] = asin(value);
    }
  } else if (strcmp(word,"acos") == 0) {
    if (tree) newtree->type = ACOS;
    else {
      if (value < -1.0 || value > 1.0) 
	error->all("Arccos of invalid value in variable formula");
      argstack[nargstack++] = acos(value);
    }
  } else if (strcmp(word,"atan") == 0) {
    if (tree) newtree->type = ATAN;
    else argstack[nargstack++] = atan(value);

  } else if (strcmp(word,"ceil") == 0) {
    if (tree) newtree->type = CEIL;
    else argstack[nargstack++] = ceil(value);

  } else if (strcmp(word,"floor") == 0) {
    if (tree) newtree->type = FLOOR;
    else argstack[nargstack++] = floor(value);

  } else if (strcmp(word,"round") == 0) {
    if (tree) newtree->type = ROUND;
    else argstack[nargstack++] = MYROUND(value);
  }

  return 1;
}

/* ----------------------------------------------------------------------
   process a object function in formula
   push result onto tree or arg stack
   word = object
   contents = str bewteen parentheses, passed to object
   return 0 if not a match, 1 if successfully processed
------------------------------------------------------------------------- */

int Variable::object_function(char *word, char *contents, Tree **tree,
			      Tree **treestack, int &ntreestack,
			      double *argstack, int &nargstack)
{
  if (!obj->find_object(word,-1)) return 0;

  Tree *newtree;
  if (tree) {
    newtree = new Tree();
    newtree->type = VALUE;
    newtree->left = newtree->right = NULL;
    treestack[ntreestack++] = newtree;
  }
    
  double value;
  int flag = obj->variable_object(word,contents,value);
  if (flag) error->all("Object variable name not recognized "
		       "in variable formula");

  if (tree) newtree->value= value;
  else argstack[nargstack++] = value;

  return 1;
}

/* ----------------------------------------------------------------------
   process a keyword in formula
   customize by adding a keyword: nprocs,time
------------------------------------------------------------------------- */

int Variable::keyword(char *word, double *value)
{
  if (strcmp(word,"nprocs") == 0) {
    int nprocs;
    MPI_Comm_size(world,&nprocs);
    *value = (double) nprocs;
  } else if (strcmp(word,"time") == 0) {
    MPI_Barrier(world);
    *value = MPI_Wtime();
  } else return 1;

  return 0;
}
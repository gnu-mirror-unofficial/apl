/*
    This file is part of GNU APL, a free implementation of the
    ISO/IEC Standard 13751, "Programming Language APL, Extended"

    Copyright (C) 2008-2020  Dr. Jürgen Sauermann

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <string.h>
#include <strings.h>

#include "CDR.hh"
#include "Command.hh"
#include "Function.hh"
#include "IndexExpr.hh"
#include "IndexIterator.hh"
#include "IntCell.hh"
#include "Output.hh"
#include "PrintOperator.hh"
#include "ProcessorID.hh"
#include "QuadFunction.hh"
#include "Quad_TF.hh"
#include "Symbol.hh"
#include "Svar_signals.hh"
#include "SystemVariable.hh"
#include "UserFunction.hh"
#include "Value.hh"
#include "ValueHistory.hh"
#include "Workspace.hh"

//-----------------------------------------------------------------------------
Symbol::Symbol(Id id)
   : NamedObject(id),
     next(0),
     name(ID::get_name_UCS(id)),
     monitor_callback(0)
{
   push();
}
//-----------------------------------------------------------------------------
Symbol::Symbol(const UCS_string & ucs, Id id)
   : NamedObject(id),
     next(0),
     name(ucs),
     monitor_callback(0)
{
   push();
}
//-----------------------------------------------------------------------------
ostream &
Symbol::print(ostream & out) const
{
   return out << name;
}
//-----------------------------------------------------------------------------
ostream &
Symbol::print_verbose(ostream & out) const
{
   out << "Symbol ";
   print(out) << " " << voidP(this) << endl;

   loop(v, value_stack.size())
       {
         out << "[" << v << "] ";
         const ValueStackItem & item = value_stack[v];
         switch(item.name_class)
            {
              case NC_INVALID:
                   out << "---INVALID---" << endl;
                   break;

              case NC_UNUSED_USER_NAME:
                   out << "Unused user defined name" << endl;
                   break;

              case NC_LABEL:
                   out << "Label line " << item.sym_val.label << endl;
                   break;

              case NC_VARIABLE:
                   {
                      Value_P val = item.apl_val;
                      out << "Variable at " << voidP(val.get()) << endl;
                      val->print_properties(out, 8, false);
                      out << endl;
                   }
                   break;

              case NC_FUNCTION:
              case NC_OPERATOR:
                   {
                     Function_P fun = item.sym_val.function;
                     Assert(fun);

                     fun->print_properties(out, 4);
                     out << "    ⎕NC:            " << item.name_class << endl
                         << "    addr:           " << voidP(fun) << endl;

                     out << endl;
                   }
                   break;

              default: break;
            }
       }

   return out;
}
//-----------------------------------------------------------------------------
void
Symbol::assign(Value_P new_value, bool clone, const char * loc)
{
   Assert(+new_value);
   Assert(value_stack.size());

   if (!new_value->is_complete())
      {
        CERR << "Incomplete value at " LOC << endl;
        new_value->print_properties(CERR, 0, false);
        VH_entry::print_history(CERR, new_value.get(), LOC);
        Assert(0);
      }

ValueStackItem & vs = value_stack.back();

   switch(vs.name_class)
      {
        case NC_UNUSED_USER_NAME:
             if (clone)  new_value = new_value->clone(loc);

             vs.name_class = NC_VARIABLE;
             vs.apl_val = new_value;
             if (monitor_callback)   monitor_callback(*this, SEV_ASSIGNED);
             return;

        case NC_LABEL:
             MORE_ERROR() << "attempt to assign a value to label " << get_name();
        case NC_VARIABLE:
             if (vs.apl_val == new_value)   return;   // X←X

             if (clone)  new_value = new_value->clone(loc);

             vs.apl_val = new_value;
             if (monitor_callback)   monitor_callback(*this, SEV_ASSIGNED);
             return;

        case NC_SHARED_VAR:
             assign_shared_variable(new_value, loc);
             if (monitor_callback)   monitor_callback(*this, SEV_ASSIGNED);
             return;

        default: SYNTAX_ERROR;
      }
}
//-----------------------------------------------------------------------------
void
Symbol::assign_indexed(Value_P X, Value_P B)   // A[X] ← B
{
   // this function is called for A[X}←B when X is one-dimensional, i.e.
   // an index with no semicolons. If X contains semicolons, then
   // assign_indexed(IndexExpr IX, ...) is called instead.
   // 
const APL_Integer qio = Workspace::get_IO();

Value_P A = get_apl_value();

   if (A->is_member())
      {
        const UCS_string name(X.getref());
        Cell * data = A->get_member_data(name);
        if (data)   // member exists
           {
             if (data->is_pointer_cell() && data->get_pointer_value()->is_member())
                {
                  MORE_ERROR() << "member access: cannot override non-leaf member "
                               << name << " of variable " << get_name()
                               << ".\n      )ERASE or ⎕EX that member first.";
                  DOMAIN_ERROR;
                }
           }
        else                                       // new member
           {
             data = A->get_new_member(name);
           }
        data->release(LOC);
        data->init_from_value(B.get(), A.getref(), LOC);
        return;
      }

const ShapeItem max_idx = A->element_count();
   if (+X && X->is_scalar() && B->is_scalar() && A->get_rank() == 1)
      {
        const APL_Integer idx = X->get_ravel(0).get_near_int() - qio;
        if (idx >= 0 && idx < max_idx)
           {
             Cell & cell = A->get_ravel(idx);
             cell.release(LOC);
             cell.init(B->get_ravel(0), A.getref(), LOC);
             return;
           }
      }

   if (A->get_rank() != 1)   RANK_ERROR;

   if (!X)   // X[] ← B
      {
        const Cell & src = B->get_ravel(0);
        loop(a, max_idx)
            {
              Cell & dest = A->get_ravel(a);
              dest.release(LOC);   // free sub-values etc (if any)
              dest.init(src, A.getref(), LOC);
            }
        if (monitor_callback)   monitor_callback(*this, SEV_ASSIGNED);
        return;
      }

const ShapeItem ec_B = B->element_count();
const ShapeItem ec_X = X->element_count();
const int incr_B = (ec_B == 1) ? 0 : 1;   // maybe scalar extend B
const Cell * cX = &X->get_ravel(0);
const Cell * cB = &B->get_ravel(0);

   if (ec_B != 1 && ec_B != ec_X)   LENGTH_ERROR;

   loop(x, ec_X)
      {
        const ShapeItem idx = cX++->get_near_int() - qio;
        if (idx < 0)          INDEX_ERROR;
        if (idx >= max_idx)   INDEX_ERROR;
        Cell & dest = A->get_ravel(idx);
        dest.release(LOC);   // free sub-values etc (if any)
        dest.init(*cB, A.getref(), LOC);

         cB += incr_B;
      }

   if (monitor_callback)   monitor_callback(*this, SEV_ASSIGNED);
}
//-----------------------------------------------------------------------------
void
Symbol::assign_indexed(IndexExpr & IX, Value_P B)   // A[IX;...] ← B
{
   if (IX.value_count() == 1 && +IX.values[0])   // one-dimensional index
      {
         assign_indexed(IX.values[0], B);
        return;
      }

   // see Value::index() for comments.

Value_P A = get_apl_value();

   if (A->get_rank() != IX.value_count())   RANK_ERROR;   // ISO p. 159

   // B must either be a scalar (and is then scalar extended to the size
   // of the updated area, or else have the shape of the concatenated index
   // items for example:
   //
   //  X:   X1    ; X2    ; X3
   //  ⍴B:  b1 b2   b3 b4   b5 b6
   //
   if (1 && !B->is_scalar())
      {
        // remove dimensions with len 1 from the shapes of X and B...
        // if we see an empty Xn then we return.
        //
        Shape B1;
        loop(b, B->get_rank())
           {
             const ShapeItem sb = B->get_shape_item(b);
             if (sb != 1)   B1.add_shape_item(sb);
           }

        Shape IX1;
        for (ShapeItem ix = IX.value_count() - 1; ix >= 0; --ix)
            {
              const Value * ix_val = IX.values[ix].get();
              if (ix_val)   // normal index
                 {
                   if (ix_val->element_count() == 0)   return;   // empty index
                   loop(xx, ix_val->get_rank())
                      {
                        const ShapeItem sxx = ix_val->get_shape_item(xx);
                        if (sxx != 1)   IX1.add_shape_item(sxx);
                      }
                 }
               else     // elided index: add corresponding B dimenssion
                 {
                   if (ix >= A->get_rank())   RANK_ERROR;
                   const ShapeItem sbx =
                         A->get_shape_item(A->get_rank() - ix - 1);
                   if (sbx == 0)   return;   // empty index
                   if (sbx != 1)   IX1.add_shape_item(sbx);
                 }
            }

        if (B1 != IX1)
           {
             if (B1.get_rank() != IX1.get_rank())   RANK_ERROR;
             LENGTH_ERROR;
           }
      }

MultiIndexIterator mult(A->get_shape(), IX);

const ShapeItem ec_B = B->element_count();
const Cell * cB = &B->get_ravel(0);
const int incr_B = (ec_B == 1) ? 0 : 1;

   while (mult.more())
      {
        const ShapeItem offset_A = mult++;
        if (offset_A < 0)                     INDEX_ERROR;
        if (offset_A >= A->element_count())   INDEX_ERROR;
        Cell & dest = A->get_ravel(offset_A);
        dest.release(LOC);   // free sub-values etc (if any)
        dest.init(*cB, A.getref(), LOC);
        cB += incr_B;
     }

   if (monitor_callback)   monitor_callback(*this, SEV_ASSIGNED);
}
//-----------------------------------------------------------------------------
bool
Symbol::assign_named_lambda(Function_P lambda, const char * loc)
{
ValueStackItem & vs = value_stack.back();
const UserFunction * ufun = lambda->get_ufun1();
   Assert(ufun);
const Executable * uexec = ufun;
   Assert(uexec);

   switch(vs.name_class)
      {
        case NC_FUNCTION:
        case NC_OPERATOR:
             {
               Function_P old_fun = vs.sym_val.function;
               Assert(old_fun);
               if (!old_fun->is_lambda())   SYNTAX_ERROR;
               const UserFunction * old_ufun = old_fun->get_ufun1();
               Assert(old_ufun);
               for (StateIndicator * si = Workspace::SI_top();
                    si; si = si->get_parent())
                   {
//                   if (uexec == si->get_executable())
                     if (si->uses_function(old_ufun))
                        {
                          MORE_ERROR() << "function " << get_name()
                                       << " is suspended or used";
                          return true;
                        }
                   }

               const_cast<UserFunction *>(vs.sym_val.function->get_ufun1())
                         ->decrement_refcount(LOC);
             }

             /* fall through */

        case NC_UNUSED_USER_NAME:
             if (lambda->is_operator())   vs.name_class = NC_OPERATOR;
             else                         vs.name_class = NC_FUNCTION;

             vs.sym_val.function = ufun;
             const_cast<UserFunction *>(ufun)->increment_refcount(LOC);
             if (monitor_callback)   monitor_callback(*this, SEV_ASSIGNED);
             return false;

        default: SYNTAX_ERROR;
      }

   return false;
}
//-----------------------------------------------------------------------------
void
Symbol::pop()
{
   if (value_stack.size() == 0)
      {
        CERR << "Symbol is: '" << get_name() << "' at " << LOC << endl;
        FIXME;
      }

const ValueStackItem & vs = value_stack.back();

   if (vs.name_class == NC_VARIABLE)
      {
        Log(LOG_SYMBOL_push_pop)
           {
             Value_P ret = vs.apl_val;
             CERR << "-pop-value " << name
                  << " flags " << ret->get_flags() << " ";
             if (value_stack.size() == 0)   CERR << " (last)";
             CERR << " addr " << voidP(ret.get()) << endl;
           }

        value_stack.pop_back();
        if (monitor_callback)   monitor_callback(*this, SEV_POPED);
      }
   else
      {
        Log(LOG_SYMBOL_push_pop)
           {
             CERR << "-pop " << name
                  << " name_class " << vs.name_class << " ";
             if (value_stack.size() == 0)   CERR << " (last)";
             CERR << endl;
           }
        value_stack.pop_back();
        if (monitor_callback)   monitor_callback(*this, SEV_POPED);
      }
}
//-----------------------------------------------------------------------------
void
Symbol::push()
{
   Log(LOG_SYMBOL_push_pop)
      {
        CERR << "+push " << name;
        if (value_stack.size() == 0)   CERR << " (initial)";
        CERR << endl;
      }

   value_stack.push_back(ValueStackItem());
   if (monitor_callback)   monitor_callback(*this, SEV_PUSHED);
}
//-----------------------------------------------------------------------------
void
Symbol::push_label(Function_Line label)
{
   Log(LOG_SYMBOL_push_pop)
      {
        CERR << "+push_label " << name;
        if (value_stack.size() == 0)   CERR << " (initial)";
        CERR << endl;
      }

   value_stack.push_back(ValueStackItem(label));
   if (monitor_callback)   monitor_callback(*this, SEV_PUSHED);
}
//-----------------------------------------------------------------------------
void
Symbol::push_function(Function_P function)
{
   Log(LOG_SYMBOL_push_pop)
      {
        CERR << "+push_function " << name << " " << voidP(function);
        if (value_stack.size() == 0)   CERR << " (initial)";
        CERR << endl;
      }

ValueStackItem vs;
   if (function->is_operator())   vs.name_class = NC_OPERATOR;
   else                           vs.name_class = NC_FUNCTION;
   vs.sym_val.function = function;
   value_stack.push_back(vs);
   if (monitor_callback)   monitor_callback(*this, SEV_PUSHED);
}
//-----------------------------------------------------------------------------
void
Symbol::push_value(Value_P value)
{
ValueStackItem vs;
   value_stack.push_back(vs);
   if (monitor_callback)   monitor_callback(*this, SEV_PUSHED);
   assign(value, true, LOC);

   Log(LOG_SYMBOL_push_pop)
      {
        CERR << "+push-value " << name << " flags ";
        print_flags(CERR, get_value()->get_flags()) << " ";
        if (value_stack.size() == 0)   CERR << " (initial)";
        CERR << " addr " << voidP(get_value().get()) << endl;
      }
}
//-----------------------------------------------------------------------------
int
Symbol::get_ufun_depth(const UserFunction * ufun)
{
const Function * fun = ufun;
const int sym_stack_size = value_stack_size();

   loop(s, sym_stack_size)
      {
        const ValueStackItem & vsi = value_stack[s];
        if (vsi.name_class != NC_FUNCTION &&
            vsi.name_class != NC_OPERATOR)   continue;
        if (fun != vsi.sym_val.function)     continue;

       // found at level s
       //
       return s;
      }

   // not found: return -1
   return -1;
}
//-----------------------------------------------------------------------------
Value_P
Symbol::get_value()
{
   if (value_stack.size())
      {
        const ValueStackItem & vs = value_stack.back();
        if (vs.name_class == NC_VARIABLE)   return vs.apl_val;
      }

   return Value_P();
}
//-----------------------------------------------------------------------------
const char *
Symbol::cant_be_defined() const
{
// if (value_stack.size() > 1)         return "symbol was localized";
   if (Workspace::is_called(name))
      return "function is called (used on the )SI stack). Try )SIC first.";

   if (value_stack.back().name_class == NC_UNUSED_USER_NAME)   return 0;   // OK
   if (value_stack.back().name_class == NC_FUNCTION)           return 0;   // OK
   if (value_stack.back().name_class == NC_OPERATOR)           return 0;   // OK
   return "bad name class";
}
//-----------------------------------------------------------------------------
Value_P
Symbol::get_apl_value() const
{
   Assert(value_stack.size() > 0);
   if (value_stack.back().name_class != NC_VARIABLE)
      Error::throw_symbol_error(get_name(), LOC);

   return value_stack.back().apl_val;
}
//-----------------------------------------------------------------------------
const Cell *
Symbol::get_first_cell() const
{
   Assert(value_stack.size() > 0);
   if (value_stack.back().name_class != NC_VARIABLE)   return 0;
   return &value_stack.back().apl_val->get_ravel(0);
}
//-----------------------------------------------------------------------------
bool
Symbol::can_be_assigned() const
{
   switch (value_stack.back().name_class)
      {
        case NC_UNUSED_USER_NAME:
        case NC_VARIABLE:
        case NC_SHARED_VAR: return true;
        default:            return false;
      }
}
//-----------------------------------------------------------------------------
SV_key
Symbol::get_SV_key() const
{
   Assert(value_stack.size() > 0);

   if (value_stack.back().name_class != NC_SHARED_VAR)   return SV_key(0);

   return value_stack.back().sym_val.sv_key;
}
//-----------------------------------------------------------------------------
void
Symbol::set_SV_key(SV_key key)
{
   value_stack.back().name_class = NC_SHARED_VAR;
   value_stack.back().sym_val.sv_key = key;
}
//-----------------------------------------------------------------------------
const Function *
Symbol::get_function() const
{
   Assert(value_stack.size() > 0);
   switch(value_stack.back().name_class)
      {
        case NC_FUNCTION:
        case NC_OPERATOR: return value_stack.back().sym_val.function;
        default:          return 0;
      }
}
//-----------------------------------------------------------------------------
Function_P
Symbol::get_function()
{
const ValueStackItem & vs = value_stack.back();

   if (vs.name_class == NC_FUNCTION)   return vs.sym_val.function;
   if (vs.name_class == NC_OPERATOR)   return vs.sym_val.function;

   return 0;
}
//-----------------------------------------------------------------------------
void
Symbol::get_attributes(int mode, Cell * dest) const
{
const ValueStackItem & vs = value_stack.back();
bool has_result = false;
int fun_valence = 0;
int oper_valence = 0;
double created = 0.0;
static int exec_prop[] = { 0, 0, 0, 0 };
const int * exec_properties = exec_prop;

   switch(vs.name_class)
      {
        case NC_LABEL:
        case NC_VARIABLE:
             has_result = true;
             break;

        case NC_FUNCTION:
             has_result = vs.sym_val.function->has_result();
             fun_valence = vs.sym_val.function->get_fun_valence();
             created = vs.sym_val.function->get_creation_time();
             exec_properties = vs.sym_val.function->get_exec_properties();
             break;

        case NC_OPERATOR:
             fun_valence = vs.sym_val.function->get_fun_valence();
             oper_valence = vs.sym_val.function->get_oper_valence();
             created = vs.sym_val.function->get_creation_time();
             exec_properties = vs.sym_val.function->get_exec_properties();
             break;

        default: break;
      }

   switch(mode)
      {
        case 1: // valences
                new (dest + 0) IntCell(has_result ? 1 : 0);
                new (dest + 1) IntCell(fun_valence);
                new (dest + 2) IntCell(oper_valence);
                break;

        case 2: // creation time
                if (created == 0.0)   // system function
                   {
                     new (dest + 0) IntCell(0);
                     new (dest + 1) IntCell(0);
                     new (dest + 2) IntCell(0);
                     new (dest + 3) IntCell(0);
                     new (dest + 4) IntCell(0);
                     new (dest + 5) IntCell(0);
                     new (dest + 6) IntCell(0);
                   }
                else                       // user define function
                   {
                     tm tc;
                     const time_t time = created;
                     localtime_r(&time, &tc);

                     new (dest + 0) IntCell(1900 + tc.tm_year);
                     new (dest + 1) IntCell(   1 + tc.tm_mon );
                     new (dest + 2) IntCell(       tc.tm_mday);
                     new (dest + 3) IntCell(       tc.tm_hour);
                     new (dest + 4) IntCell(       tc.tm_min);
                     new (dest + 5) IntCell(       tc.tm_sec);
                     new (dest + 6) IntCell((created - time) / 1000);
                   }
                break;

        case 3: // execution properties
                new (dest + 0) IntCell(exec_properties[0]);
                new (dest + 1) IntCell(exec_properties[1]);
                new (dest + 2) IntCell(exec_properties[2]);
                new (dest + 3) IntCell(exec_properties[3]);
                break;

        case 4: {
                  Value_P val = get_apl_value();
                  const CDR_type cdr_type = val->get_CDR_type();
                  const int brutto = val->total_size_brutto(cdr_type);
                  const int data = val->data_size(cdr_type);

                  new (dest + 0) IntCell(brutto);
                  new (dest + 1) IntCell(data);
                }
                break;

        default:  Assert(0 && "bad mode");
      }
}
//-----------------------------------------------------------------------------
void
Symbol::resolve(Token & tok, bool left_sym)
{
   Log(LOG_SYMBOL_resolve)
      CERR << "resolve(" << left_sym << ") symbol " << get_name() << endl; 

   Assert1(value_stack.size());

const ValueStackItem & vs = value_stack.back();
   switch(vs.name_class)
      {
        case NC_UNUSED_USER_NAME:
             if (!left_sym)   Error::throw_symbol_error(get_name(), LOC);
             return;   // leave symbol as is

        case NC_LABEL:
             if (left_sym)   SYNTAX_ERROR;   // assignment to (read-only) label

             {
               IntCell lab(vs.sym_val.label);
               Value_P value(lab, LOC);
               Token t(TOK_APL_VALUE1, value);
               tok.move_1(t, LOC);
             }
             return;

        case NC_VARIABLE:
             if (left_sym)   return;   // leave symbol as is

             // if we resolve a variable. the value is considered grouped.
             {
               Token t(TOK_APL_VALUE1, get_apl_value()->clone(LOC));
               tok.move_1(t, LOC);
             }
             return;

        case NC_FUNCTION:
        case NC_OPERATOR:
             if (left_sym && vs.sym_val.function->is_lambda())
                {
                  // lambda re-assign, e.g. SYM←{ ... }
                  //
                  return;
                }
             tok.move_2(vs.sym_val.function->get_token(), LOC);
             return;

        case NC_SHARED_VAR:
             if (left_sym)   return;   // leave symbol as is
             resolve_shared_variable(tok);
             return;

        default:
             CERR << "Symbol is '" << get_name() << "' at " << LOC << endl;
             SYNTAX_ERROR;
      }
}
//-----------------------------------------------------------------------------
Token
Symbol::resolve_lv(const char * loc)
{
   Log(LOG_SYMBOL_resolve)
      CERR << "resolve_lv() symbol " << get_name() << endl; 

   Assert(value_stack.size());

   // if this is not a variable, then re-use the error handling of resolve().
   if (value_stack.back().name_class != NC_VARIABLE)
      {
        CERR << "Symbol '" << get_name()
             << "' has changed type from variable to name class "
             << value_stack.back().name_class << endl
             << " while executing an assignment" << endl;
        throw_apl_error(E_LEFT_SYNTAX_ERROR, loc);
      }

Value_P val = value_stack.back().apl_val;
   return Token(TOK_APL_VALUE1, val->get_cellrefs(loc));
}
//-----------------------------------------------------------------------------
TokenClass
Symbol::resolve_class(bool left)
{
   Assert1(value_stack.size());

   switch(value_stack.back().name_class)
      {
        case NC_LABEL:
        case NC_VARIABLE:
        case NC_SHARED_VAR:
             return (left) ? TC_SYMBOL : TC_VALUE;

        case NC_FUNCTION:
             {
               const int valence = value_stack.back().sym_val.function
                                 ->get_fun_valence();
               if (valence == 2)   return TC_FUN2;
               if (valence == 1)   return TC_FUN1;
               return TC_FUN0;
             }

        case NC_OPERATOR:
             {
               const int valence = value_stack.back().sym_val.function
                                 ->get_oper_valence();
               return (valence == 2) ? TC_OPER2 : TC_OPER1;
             }

        default: return TC_SYMBOL;
      }
}
//-----------------------------------------------------------------------------
int
Symbol::expunge()
{
   if (value_stack.size() == 0)   return 1;   // empty stack

ValueStackItem & vs = value_stack.back();

   if (vs.name_class == NC_VARIABLE)
      {
        ptr_clear(vs.apl_val, LOC);
      }
   else if (vs.name_class == NC_FUNCTION || vs.name_class == NC_OPERATOR)
      {
        if (vs.sym_val.function->is_native())
           {
             // do not delete native functions
           }
        else if (vs.sym_val.function->is_lambda())
           {
             const UserFunction * ufun = vs.sym_val.function->get_ufun1();
             Assert(ufun);
               const_cast<UserFunction *>(vs.sym_val.function->get_ufun1())
                          ->decrement_refcount(LOC);
           }
        else
           {
             const UserFunction * ufun = vs.sym_val.function->get_ufun1();
             const Executable * exec = ufun;
             StateIndicator * oexec = Workspace::oldest_exec(exec);
             if (oexec)
                {
                  // ufun is still used on the SI stack. We do not delete ufun,
                  // but merely remember it for deletion later on.
                  //
                  // CERR << "⎕EX function " << ufun->get_name()
                  //      << " is on SI !" << endl;
                  Workspace::add_expunged_function(ufun);
                }
             else
                {
                  delete ufun;
                }
           }
        vs.name_class = NC_UNUSED_USER_NAME;
      }

   vs.clear();
   call_monitor_callback(SEV_ERASED);
   return 1;
}
//-----------------------------------------------------------------------------
void
Symbol::set_nc(NameClass nc)
{
ValueStackItem & vs = value_stack.back();

   if (vs.name_class == NC_UNUSED_USER_NAME)
      {
        vs.name_class = nc;
        return;
      }

   DEFN_ERROR;
}
//-----------------------------------------------------------------------------
void
Symbol::share_var(SV_key key)
{
ValueStackItem & vs = value_stack.back();

   if (vs.name_class == NC_UNUSED_USER_NAME)   // new shared variable
      {
        vs.name_class = NC_SHARED_VAR;
        set_SV_key(key);
        return;
      }

   if (vs.name_class == NC_VARIABLE)           // existing variable
      {
        // remember old value
        //
        Value_P old_value = get_apl_value();

        // change name class and store AP number
        //
        vs.name_class = NC_SHARED_VAR;
        set_SV_key(key);

        // assign old value to shared variable
        assign(old_value, true, LOC);

        return;
      }

   DEFN_ERROR;
}
//-----------------------------------------------------------------------------
SV_Coupling
Symbol::unshare_var()
{
   if (value_stack.size() == 0)   return NO_COUPLING;

ValueStackItem & vs = value_stack.back();
   if (vs.name_class != NC_SHARED_VAR)   return NO_COUPLING;

const SV_key key = get_SV_key();
const SV_Coupling old_coupling = Svar_DB::get_coupling(key);

   Svar_DB::retract_var(key);

   set_SV_key(0);
   vs.name_class = NC_UNUSED_USER_NAME;

   return old_coupling;
}
//-----------------------------------------------------------------------------
void
Symbol::set_nc(NameClass nc, Function_P fun)
{
ValueStackItem & vs = value_stack.back();

const bool can_set = (vs.name_class == NC_FUNCTION) ||
                     (vs.name_class == NC_OPERATOR) ||
                     (vs.name_class == NC_UNUSED_USER_NAME);

   Assert(nc == NC_FUNCTION || nc == NC_OPERATOR || nc == NC_UNUSED_USER_NAME);

   if (!can_set)   DEFN_ERROR;
   vs.sym_val.function = fun;
   if (fun)   vs.name_class = nc;
   else       vs.name_class = NC_UNUSED_USER_NAME;
}
//-----------------------------------------------------------------------------
ostream &
Symbol::list(ostream & out)
{
   out << "   ";
   loop(s, name.size())   out << name[s];

   for (int s = name.size(); s < 32; ++s)   out << " ";

   if (is_erased())   out << "   ERASED";
   Assert(value_stack.size());
const NameClass nc = value_stack.back().name_class;
   if      (nc == NC_INVALID)            out << "   INVALID NC";
   else if (nc == NC_UNUSED_USER_NAME)   out << "   Unused";
   else if (nc == NC_LABEL)              out << "   Label";
   else if (nc == NC_VARIABLE)           out << "   Variable";
   else if (nc == NC_FUNCTION)           out << "   Function";
   else if (nc == NC_OPERATOR)           out << "   Operator";
   else                                  out << "   !!! Error !!!";

   return out << endl;
}
//-----------------------------------------------------------------------------
void
Symbol::write_OUT(FILE * out, uint64_t & seq) const
{
const NameClass nc = value_stack[0].name_class;

char buffer[128];   // a little bigger than needed - don't use sizeof(buffer)
UCS_string data;

   switch(nc)
      {
        case NC_VARIABLE:
             {
               data.append(UNI_A);
               Quad_TF::tf2_var(get_name(), value_stack[0].apl_val);
             }
             break;

        case NC_FUNCTION:
        case NC_OPERATOR:
             {
               // write a timestamp record
               //
               const Function & fun = *value_stack[0].sym_val.function;
               const YMDhmsu ymdhmsu(fun.get_creation_time());
               sprintf(buffer, "*(%d %d %d %d %d %d %d)",
                       ymdhmsu.year, ymdhmsu.month, ymdhmsu.day,
                       ymdhmsu.hour, ymdhmsu.minute, ymdhmsu.second,
                       ymdhmsu.micro/1000);

               for (char * cp = buffer + strlen(buffer);
                    cp < (buffer + 72); )   *cp++ = ' ';
                sprintf(buffer + 72, "%8.8lld\r\n", long_long(seq++));
                fwrite(buffer, 1, 82, out);

               // write function record(s)
               //
               data.append(UNI_F);
               data.append(get_name());
               data.append(UNI_SPACE);
               Quad_TF::tf2_fun_ucs(data, get_name(), fun);
             }
             break;

        default: return;
      }

   for (ShapeItem u = 0; u < data.size() ;)
      {
        const ShapeItem rest = data.size() - u;
        if (rest <= 71)   buffer[0] = 'X';   // last record
        else              buffer[0] = ' ';   // more records
        loop(uu, 71)
           {
             unsigned char cc = ' ';
             if (u < data.size()) cc = Avec::unicode_to_cp(data[u++]);
             buffer[1 + uu] = cc;
           }

        sprintf(buffer + 72, "%8.8lld\r\n", long_long(seq++));
        fwrite(buffer, 1, 82, out);
      }
}
//-----------------------------------------------------------------------------
void
Symbol::unmark_all_values() const
{
   loop(v, value_stack.size())
       {
         const ValueStackItem & item = value_stack[v];
         switch(item.name_class)
            {
              case NC_VARIABLE:
                   item.apl_val->unmark();
                   break;

              case NC_FUNCTION:
              case NC_OPERATOR:
                   if (item.sym_val.function->is_native())   break;
                   {
                     const UserFunction * ufun =
                                          item.sym_val.function->get_ufun1();

                     // ufun can be 0 for example if F is a function argument
                     // of a defined operator and F is a primitive function
                     //
                     if (ufun)   ufun->unmark_all_values();
                   }
                   break;

              default: break;
            }
       }
}
//-----------------------------------------------------------------------------
int
Symbol::show_owners(ostream & out, const Value & value) const
{
int count = 0;

   loop(v, value_stack.size())
       {
         const ValueStackItem & item = value_stack[v];
         switch(item.name_class)
            {
              case NC_VARIABLE:
                   if (Value::is_or_contains(item.apl_val.get(), value))
                      {
                         out << "    Variable[vs=" << v << "] "
                            << get_name() << endl;
                         ++count;
                      }
                   break;

              case NC_FUNCTION:
              case NC_OPERATOR:
                   {
                     const Function * fun = item.sym_val.function;
                     const Executable * ufun = fun->get_ufun1();
                     Assert(ufun || fun->is_native());
                     if (ufun)
                        {
                          char cc[100];
                          snprintf(cc, sizeof(cc), "    VS[%lld] ",
                                   long_long(v));
                          count += ufun->show_owners(cc, out, value);
                        }
                   }
                   break;

              default: break;
            }
       }

   return count;
}
//-----------------------------------------------------------------------------
void
Symbol::vector_assignment(std::vector<Symbol *> & symbols, Value_P values)
{
   if (values->get_rank() > 1)   RANK_ERROR;
   if (!values->is_scalar() &&
       size_t(values->element_count()) != symbols.size())   LENGTH_ERROR;

const int incr = values->is_scalar() ? 0 : 1;
const Cell * cV = &values->get_ravel(0);
   loop(s, symbols.size())
      {
        Symbol * sym = symbols[symbols.size() - s - 1];
        if (cV->is_pointer_cell())
           {
             sym->assign(cV->get_pointer_value(), true, LOC);
           }
        else
           {
             Value_P val(LOC);
             val->next_ravel()->init(*cV, val.getref(), LOC);
             val->check_value(LOC);
             sym->assign(val, true, LOC);
           }

        cV += incr;   // scalar extend values
      }
}
//-----------------------------------------------------------------------------
void
Symbol::dump(ostream & out) const
{
const ValueStackItem & vs = value_stack[0];
   if (vs.name_class == NC_VARIABLE)
      {
        UCS_string_vector CR10;
        const Value & value = vs.apl_val.getref();
        Quad_CR::do_CR10_variable(CR10, get_name(), value);

        if (value.is_member())
           out << "⍝ structured variable " << get_name() << endl;

        loop(l, CR10.size())
           {
             if (l || value.is_member())   out << "  ";
             out << CR10[l] << endl;
           }

        if (value.is_member())
           out << "⍝ end of structured variable " << get_name() << endl;

        out << endl;
      }
   else if (vs.name_class == NC_FUNCTION ||
            vs.name_class == NC_OPERATOR)
      {
        const Function * fun = vs.sym_val.function;
        if (fun == 0)
           {
             out << "⍝ function " << get_name() << " has function pointer 0!"
                 << endl << endl;
             return;
           }

        const UserFunction * ufun = fun->get_ufun1();
        if (ufun == 0)
           {
             out << "⍝ function " << get_name() << " has ufun1 pointer 0!"
                 << endl << endl;
             return;
           }

        const UCS_string text = ufun->canonical(false);
        if (ufun->is_lambda())
           {
             out << get_name();
             out << "←{";
             int t = 0;
             while (t < text.size())   // skip λ header
                {
                  const Unicode uni = text[t++];
                  if (uni == UNI_LF)   break;
                }

             // skip λ← and spaces
             while (t < text.size() && text[t] <= ' ')   ++t;
             if    (t < text.size() && text[t] == UNI_LAMBDA)   ++t;
             while (t < text.size() && text[t] <= ' ')   ++t;
             if    (t < text.size() && text[t] == UNI_LEFT_ARROW)   ++t;
             while (t < text.size() && text[t] <= ' ')   ++t;

             // copy body
             //
             while (t < text.size())
                {
                   const Unicode uni = text[t++];
                   if (uni == UNI_LF)   break;
                   out << uni;
                }

             // append local variables
             //
             loop(l, ufun->local_var_count())
                 {
                   out << ";" << ufun->get_local_var(l)->get_name();
                 }
             out << UNI_R_CURLY << endl;
           }
        else
           {
             UCS_string_vector lines;
             text.to_vector(lines);
             out << "∇";
             loop(l, lines.size())
                {
                  UCS_string & line = lines[l];
                  line.remove_leading_and_trailing_whitespaces();
                  if (l)   out << " ";
                  out << line << endl;
                }

             if (ufun->get_exec_properties()[0])   out << "⍫";
             else                                  out << "∇";
             out << endl << endl;
           }
      }
}
//-----------------------------------------------------------------------------
int
Symbol::get_SI_level(const Function * fun) const
{
   loop(v, value_stack.size())
       {
         const ShapeItem from_tos = value_stack.size() - v - 1;
         const ValueStackItem & item = value_stack[from_tos];
         if (item.sym_val.function == fun)
            return Workspace::SI_top()->nth_push(this, from_tos);
       }

   FIXME;
}
//-----------------------------------------------------------------------------
int
Symbol::get_SI_level(const Value * val) const
{
   loop(v, value_stack.size())
       {
         const ShapeItem from_tos = value_stack.size() - v - 1;
         const ValueStackItem & item = value_stack[from_tos];
         if (item.apl_val.get() == val)
            return Workspace::SI_top()->nth_push(this, from_tos);
       }

   FIXME;
}
//-----------------------------------------------------------------------------
void
Symbol::clear_vs()
{
   while (value_stack.size() > 1)
      {
        pop();
      }

ValueStackItem & tos = value_stack[0];

   switch(tos.name_class)
      {
        case NC_LABEL:
             // should not happen since stack height == 1"
             FIXME;
             break;

        case NC_VARIABLE:
             tos.name_class = NC_UNUSED_USER_NAME;
             ptr_clear(tos.apl_val, LOC);
             break;

        case NC_UNUSED_USER_NAME:
             break;

        case NC_FUNCTION:
        case NC_OPERATOR:
             const_cast<Function *>(tos.sym_val.function)->destroy();
             tos.name_class = NC_UNUSED_USER_NAME;
             break;

        default: break;
      }
}
//-----------------------------------------------------------------------------
ostream &
operator <<(ostream & out, const Symbol & sym)
{
   return sym.print(out);
}
//-----------------------------------------------------------------------------
void
Symbol::assign_shared_variable(Value_P new_value, const char * loc)
{
   // put new_value into a CDR string
   //
CDR_string cdr;
   CDR::to_CDR(cdr, *new_value);
   if (cdr.size() > MAX_SVAR_SIZE)   LIMIT_ERROR_SVAR;

string data(charP(cdr.get_items()), cdr.size());

   // wait for shared variable to be ready
   //
const bool ws_to_ws = Svar_DB::is_ws_to_ws(get_SV_key());

   for (int w = 0; ; ++w)
       {
         if (Svar_DB::may_set(get_SV_key(), w))   // ready for writing
            {
              if (w)
                 {
                   Log(LOG_shared_variables)
                      CERR << " - OK." << endl;
                 }
              break;
            }

         if (w == 0)
            {
              Log(LOG_shared_variables)
                 {
                   CERR << "Shared variable ";
                   for (const uint32_t * varname =
                                         Svar_DB::get_svar_name(get_SV_key());
                        varname && *varname; ++varname)
                       CERR << Unicode(*varname++);
                   CERR << " is blocked on set. Waiting ...";
                 }
            }
         else if (w%25 == 0)
            {
              Log(LOG_shared_variables)   CERR << ".";
            }

         usleep(10000);   // wait 10 ms
       }

const TCP_socket tcp = Svar_DB::get_DB_tcp();

   if (ws_to_ws)
      {
        // variable shared between workspaces. It is stored on APserver
        //

        // update shared var state (BEFORE sending request to peer)
        //
        ASSIGN_WSWS_VAR_c(tcp, get_SV_key(), data);
        return;
      }

   // update shared var state (BEFORE sending request to peer)
   //
   Svar_DB::set_state(get_SV_key(), false, loc);

   ASSIGN_VALUE_c request(tcp, get_SV_key(), data);

char * del = 0;
char buffer[2*MAX_SIGNAL_CLASS_SIZE];
const char * err_loc = 0;
const Signal_base * response =
      Signal_base::recv_TCP(tcp, buffer, sizeof(buffer), del, 0, &err_loc);

   if (response == 0)
      {
        cerr << "TIMEOUT on signal ASSIGN_VALUE" << endl;
        if (del)   delete del;
        VALUE_ERROR;
      }

const ErrorCode ec = ErrorCode(response->get__SVAR_ASSIGNED__error());
   if (ec)
      {
        Log(LOG_shared_variables)
           {
             Error e(ec, response->get__SVAR_ASSIGNED__error_loc().c_str());
             cerr << Error::error_name(ec) << " assigning "
                  << get_name() << ", detected at "
                  << response->get__SVAR_ASSIGNED__error_loc()
                  << endl;
           }

        delete response;
        if (del)   delete del;
        throw_apl_error(ec, loc);
      }

   delete response;
   if (del)   delete del;
}
//-----------------------------------------------------------------------------
void
Symbol::resolve_shared_variable(Token & tok)
{
   // wait for shared variable to be ready
   //
const bool ws_to_ws = Svar_DB::is_ws_to_ws(get_SV_key());

   for (int w = 0; ; ++w)
       {
         if (Svar_DB::may_use(get_SV_key(), w))   // ready for reading
            {
              if (w)
                 {
                   Log(LOG_shared_variables)   cerr << " - OK." << endl;
                 }
              break;
            }

         if (w == 0)
            {
              Log(LOG_shared_variables)
                 {
                   CERR << "apl" << ProcessorID::get_id().proc
                                    << ": Shared variable ";
                   for (const uint32_t * varname =
                                         Svar_DB::get_svar_name(get_SV_key());
                        varname && *varname; ++varname)
                       CERR << Unicode(*varname++);
                   CERR << " is blocked on use. Waiting ...";
                 }
            }
         else if (w%25 == 0)
            {
              Log(LOG_shared_variables)   cerr << ".";
            }

         usleep(10000);   // wait 10 ms
       }

const TCP_socket tcp = Svar_DB::get_DB_tcp();

   if (ws_to_ws)
      {
        // variable shared between workspaces. It is stored on APserver
        //
        READ_WSWS_VAR_c(tcp, get_SV_key());

        char * del = 0;
        char buffer[MAX_SIGNAL_CLASS_SIZE + 40000];
        const char * err_loc = 0;
        const Signal_base * response =
              Signal_base::recv_TCP(tcp, buffer, sizeof(buffer),
                                    del, 0, &err_loc);

        if (response == 0)
           {
             if (del)   delete del;
             CERR << "no response to signal READ_WSWS_VAR" << endl;
             VALUE_ERROR;
           }

        const string & data = response->get__WSWS_VALUE_IS__cdr_value();
        if (data.size() == 0)
           {
             delete response;
             if (del)   delete del;
             CERR << "no data in signal WSWS_VALUE_IS" << endl;
             VALUE_ERROR;
           }

        CDR_string cdr;
        loop(d, data.size())   cdr.push_back(data[d]);
        delete response;
        if (del)   delete del;

        Value_P value = CDR::from_CDR(cdr, LOC);
        if (!value)     VALUE_ERROR;

        value->check_value(LOC);
        new (&tok) Token(TOK_APL_VALUE1, value);
        return;
      }

GET_VALUE_c request(tcp, get_SV_key());

char * del = 0;
char buffer[MAX_SIGNAL_CLASS_SIZE + 40000];
const char * err_loc = 0;
const Signal_base * response =
      Signal_base::recv_TCP(tcp, buffer, sizeof(buffer), del, 0, &err_loc);

   if (response == 0)
      {
        CERR << "TIMEOUT on signal GET_VALUE" << endl;
        VALUE_ERROR;
      }

const ErrorCode err(ErrorCode(response->get__VALUE_IS__error()));
   if (err)
      {
        Log(LOG_shared_variables)
           {
             cerr << Error::error_name(err) << " referencing "
                  << get_name() << ", detected at "
                  << response->get__VALUE_IS__error_loc() << endl;
           }

        string eloc = response->get__VALUE_IS__error_loc();
        delete response;
        if (del)   delete del;
        throw_apl_error(err, eloc.c_str());
      }

const string & data = response->get__VALUE_IS__cdr_value();
CDR_string cdr;
   loop(d, data.size())   cdr.push_back(data[d]);
   delete response;
   if (del)   delete del;

Value_P value = CDR::from_CDR(cdr, LOC);
   if (!value)     VALUE_ERROR;

   // update shared var state (AFTER sending request to peer)
   //
   Svar_DB::set_state(get_SV_key(), true, LOC);

   value->check_value(LOC);
   new (&tok) Token(TOK_APL_VALUE1, value);
}
//-----------------------------------------------------------------------------

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

#include "IndexExpr.hh"
#include "PrintBuffer.hh"
#include "PrintOperator.hh"
#include "Value.hh"
#include "Workspace.hh"

//-----------------------------------------------------------------------------
IndexExpr::IndexExpr(Assign_state astate, const char * loc)
   : DynamicObject(loc),
     quad_io(Workspace::get_IO()),
     rank(0),
     assign_state(astate)
{
}
//-----------------------------------------------------------------------------
IndexExpr::~IndexExpr()
{
// Q1(this)
// BACKTRACE
}
//-----------------------------------------------------------------------------
Value_P
IndexExpr::extract_value(uAxis rk)
{
   Assert1(rk < rank);

Value_P ret = values[rk];
   ptr_clear(values[rk], LOC);

   return ret;
}
//-----------------------------------------------------------------------------
void
IndexExpr::extract_all()
{
   loop(r, rank)
      {
        Value_P val = extract_value(r);
      }
}
//-----------------------------------------------------------------------------
bool
IndexExpr::check_range(const Shape & shape) const
{
   loop(r, rank)
      {
        Value_P ival = values[rank - r - 1];
        if (!ival)   continue;   // elided index

        const ShapeItem max_idx = shape.get_shape_item(r) + quad_io;
        loop(i, ival->element_count())
           {
             const APL_Integer idx = ival->get_ravel(i).get_near_int();
             if (idx < quad_io)    return true;
             if (idx >= max_idx)   return true;
           }
      }

   return false;   // OK
}
//-----------------------------------------------------------------------------
Rank
IndexExpr::get_axis(Rank max_axis) const
{
   if (rank != 1)   INDEX_ERROR;

const APL_Integer qio = Workspace::get_IO();

Value_P I = values[0];
   if (!I->is_scalar_or_len1_vector())     INDEX_ERROR;

   if (!I->get_ravel(0).is_near_int())   INDEX_ERROR;

   // if axis becomes (signed) negative then it will be (unsigned) too big.
   // Therefore we need not test for < 0.
   //
Rank axis = I->get_ravel(0).get_near_int() - qio;
   if (axis >= max_axis)   INDEX_ERROR;

   return axis;
}
//-----------------------------------------------------------------------------
void
IndexExpr::set_value(uAxis axis, Value_P val)
{
   // Note: we expect that all axes have been allocated with add(0)
   // Note also that IndexExpr is in parsing order, ie. 0 is the lowest axis.
   //
   Assert(axis < rank);
   Assert(!values[rank - axis - 1]);
   values[rank - axis - 1] = val;
}
//-----------------------------------------------------------------------------
Shape
IndexExpr::to_shape() const
{
   if (value_count() != 1)   INDEX_ERROR;

Value_P vx = values[0];
   if (!vx)                  INDEX_ERROR;

const ShapeItem xlen = vx->element_count();
const APL_Integer qio = Workspace::get_IO();
 
Shape shape;
     loop(x, xlen)
        shape.add_shape_item(vx->get_ravel(x).get_near_int() - qio);

   return shape;
}
//-----------------------------------------------------------------------------
int
IndexExpr::print_stale(ostream & out)
{
int count = 0;

   for (const DynamicObject * dob = all_index_exprs.get_next();
        dob != &all_index_exprs; dob = dob->get_next())
       {
         const IndexExpr * idx = dob->pIndexExpr();

         out << dob->where_allocated();

         try           { out << *idx; }
         catch (...)   { out << " *** corrupt ***"; }

         out << endl;

         ++count;
       }

   return count;
}
//-----------------------------------------------------------------------------
void
IndexExpr::erase_stale(const char * loc)
{
   Log(LOG_Value__erase_stale)
      CERR << endl << endl << "erase_stale() called from " << loc << endl;

   for (DynamicObject * dob = all_index_exprs.get_next();
        dob != &all_index_exprs; dob = dob->get_next())
       {
         IndexExpr * idx = dob->pIndexExpr();

         Log(LOG_Value__erase_stale)
            {
              CERR << "Erasing stale IndexExpr:" << endl
                   << "  Allocated by " << idx->alloc_loc << endl;
            }

         dob = dob->get_prev();
         idx->unlink();
         delete idx;
       }
}
//-----------------------------------------------------------------------------
ostream &
operator <<(ostream & out, const IndexExpr & idx)
{
   out << "[";
   loop(i, idx.value_count())
      {
        if (i)   out << ";";
        if (+idx.values[i])
           {
             // value::print() may print a trailing LF that we dont want here.
             //
             const PrintContext pctx = Workspace::get_PrintContext(PR_APL_MIN);
             PrintBuffer pb(*idx.values[i], pctx, 0);

             UCS_string ucs(pb, idx.values[i]->get_rank(), Workspace::get_PW());
             out << ucs;
           }
      }
   return out << "]";
}
//-----------------------------------------------------------------------------

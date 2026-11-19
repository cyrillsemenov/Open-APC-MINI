use std::collections::HashMap;
use std::slice;
use std::sync::Mutex;

use full_moon::ast::{
    Assignment, Ast, BinOp, Block, Call, Expression, FunctionCall, FunctionDeclaration, Index,
    LastStmt, LocalAssignment, NumericFor, Prefix, Stmt, Suffix, UnOp, Var, While,
};
use full_moon::tokenizer::{Symbol, TokenReference, TokenType};

pub const LUA_HEADER_SIZE: usize = 16;
pub const LUA_MAX_IMAGE_LEN: usize = 768;
pub const LUA_ENTRY_NONE: u16 = 0xFFFF;

// Opcodes (must match src/protocol/lua_vm.h and tools/lua_compiler.py)
pub const LUA_OP_HALT: u8 = 0x00;
pub const LUA_OP_RET: u8 = 0x01;
pub const LUA_OP_PUSH_I8: u8 = 0x02;
pub const LUA_OP_PUSH_I16: u8 = 0x03;
pub const LUA_OP_GET_LOCAL: u8 = 0x04;
pub const LUA_OP_SET_LOCAL: u8 = 0x05;
pub const LUA_OP_GET_GLOBAL: u8 = 0x06;
pub const LUA_OP_SET_GLOBAL: u8 = 0x07;
pub const LUA_OP_GET_MEM: u8 = 0x08;
pub const LUA_OP_SET_MEM: u8 = 0x09;
pub const LUA_OP_POP: u8 = 0x0A;

pub const LUA_OP_ADD: u8 = 0x10;
pub const LUA_OP_SUB: u8 = 0x11;
pub const LUA_OP_MUL: u8 = 0x12;
pub const LUA_OP_DIV: u8 = 0x13;
pub const LUA_OP_MOD: u8 = 0x14;
pub const LUA_OP_NEG: u8 = 0x15;
pub const LUA_OP_NOT: u8 = 0x16;
pub const LUA_OP_BAND: u8 = 0x17;
pub const LUA_OP_BOR: u8 = 0x18;
pub const LUA_OP_BXOR: u8 = 0x19;
pub const LUA_OP_SHL: u8 = 0x1A;
pub const LUA_OP_SHR: u8 = 0x1B;

pub const LUA_OP_EQ: u8 = 0x20;
pub const LUA_OP_NE: u8 = 0x21;
pub const LUA_OP_LT: u8 = 0x22;
pub const LUA_OP_LE: u8 = 0x23;
pub const LUA_OP_GT: u8 = 0x24;
pub const LUA_OP_GE: u8 = 0x25;

pub const LUA_OP_JMP: u8 = 0x30;
pub const LUA_OP_JZ: u8 = 0x31;
pub const LUA_OP_JNZ: u8 = 0x32;

pub const LUA_OP_CALL_BUILTIN: u8 = 0x40;

fn lookup_builtin(name: &str) -> Option<(u8, usize)> {
    match name {
        "led" | "g:led" | "g.led" => Some((0x00, 3)),
        "led_all" | "g:all" | "g.all" => Some((0x01, 1)),
        "grid_key" | "trigger_key" | "mext_key" => Some((0x02, 3)),
        "tilt" | "mext_tilt" => Some((0x03, 4)),
        "note_on" => Some((0x04, 3)),
        "note_off" => Some((0x05, 2)),
        "cc" => Some((0x06, 3)),
        "fader" => Some((0x07, 1)),
        "shift" => Some((0x08, 0)),
        "set_tempo" => Some((0x09, 1)),
        "rand" | "math.random" => Some((0x0A, 1)),
        "abs" | "math.abs" => Some((0x0B, 1)),
        "min" | "math.min" => Some((0x0C, 2)),
        "max" | "math.max" => Some((0x0D, 2)),
        "host_led" => Some((0x0E, 2)),
        "life_step" => Some((0x0F, 0)),
        "yield" => Some((0x10, 0)),
        "sleep_ms" => Some((0x11, 1)),
        "key_pulse" => Some((0x12, 3)),
        "fader_hires" | "fader14" => Some((0x13, 1)),
        "pitch_bend" => Some((0x14, 2)),
        "cc14" => Some((0x15, 3)),
        _ => None,
    }
}

fn token_ident(tok: &TokenReference) -> String {
    tok.token().to_string().trim().to_string()
}

fn parse_lua_integer(text: &str) -> Result<i32, String> {
    let s = text.trim();
    if let Some(hex) = s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
        i64::from_str_radix(hex, 16)
            .map(|v| v as i32)
            .map_err(|e| format!("Invalid hex integer {s:?}: {e}"))
    } else if let Ok(v) = s.parse::<i64>() {
        Ok(v as i32)
    } else if let Ok(f) = s.parse::<f64>() {
        Ok(f as i32)
    } else {
        Err(format!("Invalid number literal {s:?}"))
    }
}

pub struct LuaAstCompiler {
    code: Vec<u8>,
    globals: HashMap<String, u8>,
    locals: HashMap<String, u8>,
    next_local_slot: u8,
    entry_init: u16,
    entry_tick: u16,
    entry_grid: u16,
    entry_fader: u16,
}

impl Default for LuaAstCompiler {
    fn default() -> Self {
        Self::new()
    }
}

impl LuaAstCompiler {
    pub fn new() -> Self {
        Self {
            code: Vec::new(),
            globals: HashMap::new(),
            locals: HashMap::new(),
            next_local_slot: 0,
            entry_init: LUA_ENTRY_NONE,
            entry_tick: LUA_ENTRY_NONE,
            entry_grid: LUA_ENTRY_NONE,
            entry_fader: LUA_ENTRY_NONE,
        }
    }

    fn emit(&mut self, bytes: &[u8]) -> usize {
        let pos = self.code.len();
        self.code.extend_from_slice(bytes);
        pos
    }

    fn emit_push_int(&mut self, val: i32) {
        if (-128..=127).contains(&val) {
            self.emit(&[LUA_OP_PUSH_I8, (val as i8) as u8]);
        } else {
            let clamped = val.clamp(-32768, 32767) as i16;
            let u16_bytes = (clamped as u16).to_le_bytes();
            self.emit(&[LUA_OP_PUSH_I16, u16_bytes[0], u16_bytes[1]]);
        }
    }

    fn emit_jump(&mut self, op: u8, target: u16) -> usize {
        let t = target.to_le_bytes();
        self.emit(&[op, t[0], t[1]])
    }

    fn patch_jump(&mut self, jmp_pos: usize, target: Option<u16>) {
        let t = target.unwrap_or(self.code.len() as u16).to_le_bytes();
        self.code[jmp_pos + 1] = t[0];
        self.code[jmp_pos + 2] = t[1];
    }

    fn get_global_slot(&mut self, name: &str) -> Result<u8, String> {
        if let Some(&slot) = self.globals.get(name) {
            return Ok(slot);
        }
        if self.globals.len() >= 64 {
            return Err(format!("Too many global variables (max 64) at {name:?}"));
        }
        let slot = self.globals.len() as u8;
        self.globals.insert(name.to_string(), slot);
        Ok(slot)
    }

    fn alloc_local_slot(&mut self, name: &str) -> Result<u8, String> {
        if let Some(&slot) = self.locals.get(name) {
            return Ok(slot);
        }
        if self.next_local_slot >= 32 {
            return Err(format!(
                "Too many local variables in function (max 32) at {name:?}"
            ));
        }
        let slot = self.next_local_slot;
        self.locals.insert(name.to_string(), slot);
        self.next_local_slot += 1;
        Ok(slot)
    }

    pub fn compile_ast(&mut self, ast: &Ast) -> Result<Vec<u8>, String> {
        let block = ast.nodes();
        for stmt in block.stmts() {
            match stmt {
                Stmt::FunctionDeclaration(fn_decl) => {
                    self.compile_function(fn_decl)?;
                }
                _ => {
                    if self.entry_init == LUA_ENTRY_NONE {
                        self.entry_init = self.code.len() as u16;
                    }
                    self.compile_stmt(stmt)?;
                }
            }
        }
        if let Some(last) = block.last_stmt() {
            if self.entry_init == LUA_ENTRY_NONE {
                self.entry_init = self.code.len() as u16;
            }
            self.compile_last_stmt(last)?;
        }

        if self.code.is_empty()
            || !matches!(
                *self.code.last().unwrap(),
                LUA_OP_HALT | LUA_OP_RET
            )
        {
            self.emit(&[LUA_OP_HALT]);
        }

        let mut image = Vec::with_capacity(LUA_HEADER_SIZE + self.code.len());
        image.extend_from_slice(b"LUA1");
        image.extend_from_slice(&(self.code.len() as u16).to_le_bytes());
        image.extend_from_slice(&self.entry_init.to_le_bytes());
        image.extend_from_slice(&self.entry_tick.to_le_bytes());
        image.extend_from_slice(&self.entry_grid.to_le_bytes());
        image.extend_from_slice(&self.entry_fader.to_le_bytes());
        image.extend_from_slice(&0u16.to_le_bytes());
        image.extend_from_slice(&self.code);

        if image.len() > LUA_MAX_IMAGE_LEN {
            return Err(format!(
                "Compiled bytecode image ({} B) exceeds limit ({LUA_MAX_IMAGE_LEN} B)",
                image.len()
            ));
        }
        Ok(image)
    }

    fn compile_function(&mut self, fn_decl: &FunctionDeclaration) -> Result<(), String> {
        let mut raw_name = fn_decl
            .name()
            .names()
            .iter()
            .map(token_ident)
            .collect::<Vec<_>>()
            .join(".");
        if let Some(method) = fn_decl.name().method_name() {
            raw_name.push(':');
            raw_name.push_str(&token_ident(method));
        }

        let entry_pos = self.code.len() as u16;
        match raw_name.as_str() {
            "init" => self.entry_init = entry_pos,
            "on_tick" | "tick" => self.entry_tick = entry_pos,
            "on_grid" | "g.key" | "grid_key" => self.entry_grid = entry_pos,
            "on_fader" | "fader_move" => self.entry_fader = entry_pos,
            _ => {
                return Err(format!(
                    "Unsupported entry function {raw_name:?}; expected init, on_tick, on_grid, or on_fader"
                ))
            }
        }

        self.locals.clear();
        self.next_local_slot = 0;

        for param in fn_decl.body().parameters().iter() {
            if let full_moon::ast::Parameter::Name(tok) = param {
                let param_name = token_ident(tok);
                self.alloc_local_slot(&param_name)?;
            }
        }

        self.compile_block(fn_decl.body().block())?;
        self.emit(&[LUA_OP_HALT]);
        Ok(())
    }

    fn compile_block(&mut self, block: &Block) -> Result<(), String> {
        for stmt in block.stmts() {
            self.compile_stmt(stmt)?;
        }
        if let Some(last) = block.last_stmt() {
            self.compile_last_stmt(last)?;
        }
        Ok(())
    }

    fn compile_last_stmt(&mut self, last: &LastStmt) -> Result<(), String> {
        match last {
            LastStmt::Return(ret) => {
                let mut exprs = ret.returns().iter();
                if let Some(first) = exprs.next() {
                    self.compile_expr(first)?;
                } else {
                    self.emit_push_int(0);
                }
                self.emit(&[LUA_OP_RET]);
                Ok(())
            }
            _ => Err(format!("Unsupported last statement: {last:?}")),
        }
    }

    fn compile_stmt(&mut self, stmt: &Stmt) -> Result<(), String> {
        match stmt {
            Stmt::LocalAssignment(local_assign) => self.compile_local_assign(local_assign),
            Stmt::Assignment(assign) => self.compile_assign(assign),
            Stmt::If(if_stmt) => self.compile_if(if_stmt),
            Stmt::While(while_stmt) => self.compile_while(while_stmt),
            Stmt::NumericFor(num_for) => self.compile_numeric_for(num_for),
            Stmt::FunctionCall(call) => {
                if self.is_noop_refresh(call)? {
                    return Ok(());
                }
                self.compile_call(call)?;
                self.emit(&[LUA_OP_POP]);
                Ok(())
            }
            Stmt::Do(do_block) => self.compile_block(do_block.block()),
            _ => Err(format!("Unsupported Lua statement: {stmt}")),
        }
    }

    fn compile_local_assign(&mut self, local_assign: &LocalAssignment) -> Result<(), String> {
        let names: Vec<String> = local_assign.names().iter().map(token_ident).collect();
        let exprs: Vec<&Expression> = local_assign.expressions().iter().collect();

        for (i, name) in names.iter().enumerate() {
            if i < exprs.len() {
                self.compile_expr(exprs[i])?;
            } else {
                self.emit_push_int(0);
            }
            let slot = self.alloc_local_slot(name)?;
            self.emit(&[LUA_OP_SET_LOCAL, slot]);
        }
        Ok(())
    }

    fn compile_assign(&mut self, assign: &Assignment) -> Result<(), String> {
        let targets: Vec<&Var> = assign.variables().iter().collect();
        let exprs: Vec<&Expression> = assign.expressions().iter().collect();

        for (i, target) in targets.iter().enumerate() {
            match target {
                Var::Name(tok) => {
                    let name = token_ident(tok);
                    if i < exprs.len() {
                        self.compile_expr(exprs[i])?;
                    } else {
                        self.emit_push_int(0);
                    }
                    if let Some(&slot) = self.locals.get(&name) {
                        self.emit(&[LUA_OP_SET_LOCAL, slot]);
                    } else {
                        let slot = self.get_global_slot(&name)?;
                        self.emit(&[LUA_OP_SET_GLOBAL, slot]);
                    }
                }
                Var::Expression(var_expr) => {
                    self.compile_var_index_addr(var_expr)?;
                    if i < exprs.len() {
                        self.compile_expr(exprs[i])?;
                    } else {
                        self.emit_push_int(0);
                    }
                    self.emit(&[LUA_OP_SET_MEM]);
                }
                _ => return Err(format!("Unsupported assignment target: {target}")),
            }
        }
        Ok(())
    }

    fn compile_var_index_addr(
        &mut self,
        var_expr: &full_moon::ast::VarExpression,
    ) -> Result<(), String> {
        let suffixes: Vec<&Suffix> = var_expr.suffixes().collect();
        match suffixes.as_slice() {
            [Suffix::Index(Index::Brackets { expression, .. })] => {
                self.compile_expr(expression)?;
                Ok(())
            }
            [Suffix::Index(Index::Brackets { expression: y_expr, .. }), Suffix::Index(Index::Brackets { expression: x_expr, .. })] => {
                // 2D sugar: grid[y][x] -> (y) * 8 + (x)
                self.compile_expr(y_expr)?;
                self.emit_push_int(8);
                self.emit(&[LUA_OP_MUL]);
                self.compile_expr(x_expr)?;
                self.emit(&[LUA_OP_ADD]);
                Ok(())
            }
            _ => Err(format!("Unsupported indexed memory expression: {var_expr}")),
        }
    }

    fn compile_if(&mut self, if_stmt: &full_moon::ast::If) -> Result<(), String> {
        self.compile_expr(if_stmt.condition())?;
        let mut jz_next = Some(self.emit_jump(LUA_OP_JZ, 0));
        let mut end_jumps: Vec<usize> = Vec::new();

        self.compile_block(if_stmt.block())?;

        if let Some(else_ifs) = if_stmt.else_if() {
            for else_if in else_ifs {
                end_jumps.push(self.emit_jump(LUA_OP_JMP, 0));
                if let Some(jz) = jz_next.take() {
                    self.patch_jump(jz, None);
                }
                self.compile_expr(else_if.condition())?;
                jz_next = Some(self.emit_jump(LUA_OP_JZ, 0));
                self.compile_block(else_if.block())?;
            }
        }

        if let Some(else_block) = if_stmt.else_block() {
            end_jumps.push(self.emit_jump(LUA_OP_JMP, 0));
            if let Some(jz) = jz_next.take() {
                self.patch_jump(jz, None);
            }
            self.compile_block(else_block)?;
        }

        if let Some(jz) = jz_next.take() {
            self.patch_jump(jz, None);
        }
        for j in end_jumps {
            self.patch_jump(j, None);
        }
        Ok(())
    }

    fn compile_while(&mut self, while_stmt: &While) -> Result<(), String> {
        let loop_start = self.code.len() as u16;
        self.compile_expr(while_stmt.condition())?;
        let jz_exit = self.emit_jump(LUA_OP_JZ, 0);
        self.compile_block(while_stmt.block())?;
        self.emit_jump(LUA_OP_JMP, loop_start);
        self.patch_jump(jz_exit, None);
        Ok(())
    }

    fn eval_const_int_expr(expr: &Expression) -> Result<i32, String> {
        match expr {
            Expression::Number(tok) => parse_lua_integer(&token_ident(tok)),
            Expression::UnaryOperator {
                unop: UnOp::Minus(_),
                expression,
            } => Ok(-Self::eval_const_int_expr(expression)?),
            Expression::Parentheses { expression, .. } => Self::eval_const_int_expr(expression),
            _ => Err("For-loop step must be an integer constant".to_string()),
        }
    }

    fn compile_numeric_for(&mut self, num_for: &NumericFor) -> Result<(), String> {
        let var_name = token_ident(num_for.index_variable());
        let var_slot = self.alloc_local_slot(&var_name)?;
        let limit_slot = self.alloc_local_slot(&format!("__lim_{var_name}"))?;

        self.compile_expr(num_for.start())?;
        self.emit(&[LUA_OP_SET_LOCAL, var_slot]);
        self.compile_expr(num_for.end())?;
        self.emit(&[LUA_OP_SET_LOCAL, limit_slot]);

        let step_val = if let Some(step_expr) = num_for.step() {
            Self::eval_const_int_expr(step_expr)?
        } else {
            1
        };

        let loop_start = self.code.len() as u16;
        self.emit(&[LUA_OP_GET_LOCAL, var_slot]);
        self.emit(&[LUA_OP_GET_LOCAL, limit_slot]);
        self.emit(&[if step_val >= 0 { LUA_OP_LE } else { LUA_OP_GE }]);
        let jz_exit = self.emit_jump(LUA_OP_JZ, 0);

        self.compile_block(num_for.block())?;

        self.emit(&[LUA_OP_GET_LOCAL, var_slot]);
        self.emit_push_int(step_val);
        self.emit(&[LUA_OP_ADD]);
        self.emit(&[LUA_OP_SET_LOCAL, var_slot]);
        self.emit_jump(LUA_OP_JMP, loop_start);
        self.patch_jump(jz_exit, None);
        Ok(())
    }

    fn resolve_call<'a>(
        &self,
        call: &'a FunctionCall,
    ) -> Result<(String, Vec<&'a Expression>), String> {
        let mut base = match call.prefix() {
            Prefix::Name(tok) => token_ident(tok),
            _ => return Err(format!("Unsupported call prefix: {}", call.prefix())),
        };

        let suffixes: Vec<&Suffix> = call.suffixes().collect();
        if suffixes.is_empty() {
            return Err(format!("Malformed function call: {call}"));
        }

        for s in &suffixes[..suffixes.len() - 1] {
            match s {
                Suffix::Index(Index::Dot { name, .. }) => {
                    base.push('.');
                    base.push_str(&token_ident(name));
                }
                _ => return Err(format!("Unsupported call suffix chain in {call}")),
            }
        }

        let last = suffixes[suffixes.len() - 1];
        let fn_args = match last {
            Suffix::Call(Call::AnonymousCall(args)) => args,
            Suffix::Call(Call::MethodCall(method_call)) => {
                base.push(':');
                base.push_str(&token_ident(method_call.name()));
                method_call.args()
            }
            _ => return Err(format!("Expected call arguments at end of {call}")),
        };

        let arg_exprs: Vec<&Expression> = match fn_args {
            full_moon::ast::FunctionArgs::Parentheses { arguments, .. } => {
                arguments.iter().collect()
            }
            _ => return Err(format!("Function arguments must use parentheses in {call}")),
        };

        Ok((base, arg_exprs))
    }

    fn is_noop_refresh(&self, call: &FunctionCall) -> Result<bool, String> {
        let (fn_name, _) = self.resolve_call(call)?;
        Ok(matches!(
            fn_name.as_str(),
            "g:refresh" | "g.refresh" | "refresh"
        ))
    }

    fn compile_call(&mut self, call: &FunctionCall) -> Result<(), String> {
        let (fn_name, args) = self.resolve_call(call)?;
        let Some((fn_id, expected_argc)) = lookup_builtin(&fn_name) else {
            return Err(format!("Unknown built-in function {fn_name:?}"));
        };
        if args.len() != expected_argc {
            return Err(format!(
                "Function {fn_name}() expects {expected_argc} arguments, got {}",
                args.len()
            ));
        }
        for arg in args {
            self.compile_expr(arg)?;
        }
        self.emit(&[LUA_OP_CALL_BUILTIN, fn_id, expected_argc as u8]);
        Ok(())
    }

    fn compile_expr(&mut self, expr: &Expression) -> Result<(), String> {
        match expr {
            Expression::Number(tok) => {
                let n = parse_lua_integer(&token_ident(tok))?;
                self.emit_push_int(n);
                Ok(())
            }
            Expression::Symbol(tok) => match tok.token().token_type() {
                TokenType::Symbol {
                    symbol: Symbol::True,
                } => {
                    self.emit_push_int(1);
                    Ok(())
                }
                TokenType::Symbol {
                    symbol: Symbol::False | Symbol::Nil,
                } => {
                    self.emit_push_int(0);
                    Ok(())
                }
                _ => Err(format!("Unsupported symbol expression: {expr}")),
            },
            Expression::Parentheses { expression, .. } => self.compile_expr(expression),
            Expression::Var(var) => match var {
                Var::Name(tok) => {
                    let name = token_ident(tok);
                    if let Some(&slot) = self.locals.get(&name) {
                        self.emit(&[LUA_OP_GET_LOCAL, slot]);
                    } else {
                        let slot = self.get_global_slot(&name)?;
                        self.emit(&[LUA_OP_GET_GLOBAL, slot]);
                    }
                    Ok(())
                }
                Var::Expression(var_expr) => {
                    self.compile_var_index_addr(var_expr)?;
                    self.emit(&[LUA_OP_GET_MEM]);
                    Ok(())
                }
                _ => Err(format!("Unsupported variable expression: {var}")),
            },
            Expression::FunctionCall(call) => self.compile_call(call),
            Expression::UnaryOperator { unop, expression } => match unop {
                UnOp::Minus(_) => {
                    if let Expression::Number(tok) = expression.as_ref() {
                        let n = parse_lua_integer(&token_ident(tok))?;
                        self.emit_push_int(-n);
                    } else {
                        self.compile_expr(expression)?;
                        self.emit(&[LUA_OP_NEG]);
                    }
                    Ok(())
                }
                UnOp::Not(_) => {
                    self.compile_expr(expression)?;
                    self.emit(&[LUA_OP_NOT]);
                    Ok(())
                }
                UnOp::Tilde(_) => {
                    self.compile_expr(expression)?;
                    self.emit_push_int(-1);
                    self.emit(&[LUA_OP_BXOR]);
                    Ok(())
                }
                _ => Err(format!("Unsupported unary operator: {expr}")),
            },
            Expression::BinaryOperator { lhs, binop, rhs } => {
                match binop {
                    BinOp::And(_) => {
                        self.compile_expr(lhs)?;
                        self.compile_expr(rhs)?;
                        self.emit_push_int(0);
                        self.emit(&[LUA_OP_NE]);
                        self.emit(&[LUA_OP_BAND]);
                        return Ok(());
                    }
                    BinOp::Or(_) => {
                        self.compile_expr(lhs)?;
                        self.compile_expr(rhs)?;
                        self.emit(&[LUA_OP_BOR]);
                        return Ok(());
                    }
                    _ => {}
                }

                let opcode = match binop {
                    BinOp::Plus(_) => LUA_OP_ADD,
                    BinOp::Minus(_) => LUA_OP_SUB,
                    BinOp::Star(_) => LUA_OP_MUL,
                    BinOp::Slash(_) | BinOp::DoubleSlash(_) => LUA_OP_DIV,
                    BinOp::Percent(_) => LUA_OP_MOD,
                    BinOp::Ampersand(_) => LUA_OP_BAND,
                    BinOp::Pipe(_) => LUA_OP_BOR,
                    BinOp::Tilde(_) => LUA_OP_BXOR,
                    BinOp::DoubleLessThan(_) => LUA_OP_SHL,
                    BinOp::DoubleGreaterThan(_) => LUA_OP_SHR,
                    BinOp::TwoEqual(_) => LUA_OP_EQ,
                    BinOp::TildeEqual(_) => LUA_OP_NE,
                    BinOp::LessThan(_) => LUA_OP_LT,
                    BinOp::LessThanEqual(_) => LUA_OP_LE,
                    BinOp::GreaterThan(_) => LUA_OP_GT,
                    BinOp::GreaterThanEqual(_) => LUA_OP_GE,
                    _ => return Err(format!("Unsupported binary operator: {binop}")),
                };

                self.compile_expr(lhs)?;
                self.compile_expr(rhs)?;
                self.emit(&[opcode]);
                Ok(())
            }
            _ => Err(format!("Unsupported expression: {expr}")),
        }
    }
}

pub fn compile_lua_source(src: &str) -> Result<Vec<u8>, String> {
    let ast = full_moon::parse(src).map_err(|errs| {
        errs.into_iter()
            .map(|e| e.to_string())
            .collect::<Vec<_>>()
            .join("; ")
    })?;
    let mut compiler = LuaAstCompiler::new();
    compiler.compile_ast(&ast)
}

// ============================================================================
// WebAssembly C-ABI Exports (for wasm32-unknown-unknown without wasm-bindgen)
// ============================================================================
static LAST_OUTPUT: Mutex<Vec<u8>> = Mutex::new(Vec::new());
static LAST_ERROR: Mutex<String> = Mutex::new(String::new());

#[no_mangle]
pub extern "C" fn wasm_alloc(len: usize) -> *mut u8 {
    let mut buf = Vec::<u8>::with_capacity(len);
    let ptr = buf.as_mut_ptr();
    std::mem::forget(buf);
    ptr
}

#[no_mangle]
pub unsafe extern "C" fn wasm_free(ptr: *mut u8, len: usize) {
    if !ptr.is_null() && len > 0 {
        drop(Vec::from_raw_parts(ptr, 0, len));
    }
}

/// Compiles UTF-8 Lua 5.4 source at `(src_ptr, src_len)`.
/// Returns `0` on success (read bytecode via `wasm_output_ptr`/`wasm_output_len`)
/// or `1` on error (read UTF-8 error message via `wasm_error_ptr`/`wasm_error_len`).
#[no_mangle]
pub unsafe extern "C" fn wasm_compile_lua(src_ptr: *const u8, src_len: usize) -> u32 {
    let bytes = if src_ptr.is_null() || src_len == 0 {
        &[][..]
    } else {
        slice::from_raw_parts(src_ptr, src_len)
    };
    let src = match std::str::from_utf8(bytes) {
        Ok(s) => s,
        Err(e) => {
            *LAST_ERROR.lock().unwrap() = format!("Invalid UTF-8 source: {e}");
            return 1;
        }
    };

    match compile_lua_source(src) {
        Ok(image) => {
            *LAST_OUTPUT.lock().unwrap() = image;
            LAST_ERROR.lock().unwrap().clear();
            0
        }
        Err(err) => {
            LAST_OUTPUT.lock().unwrap().clear();
            *LAST_ERROR.lock().unwrap() = err;
            1
        }
    }
}

#[no_mangle]
pub extern "C" fn wasm_output_ptr() -> *const u8 {
    LAST_OUTPUT.lock().unwrap().as_ptr()
}

#[no_mangle]
pub extern "C" fn wasm_output_len() -> usize {
    LAST_OUTPUT.lock().unwrap().len()
}

#[no_mangle]
pub extern "C" fn wasm_error_ptr() -> *const u8 {
    LAST_ERROR.lock().unwrap().as_ptr()
}

#[no_mangle]
pub extern "C" fn wasm_error_len() -> usize {
    LAST_ERROR.lock().unwrap().len()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn compiles_all_repo_scripts() {
        let scripts = [
            include_str!("../../../scripts/bouncing_balls.lua"),
            include_str!("../../../scripts/game_of_life.lua"),
            include_str!("../../../scripts/bouncy_echo.lua"),
        ];
        for src in scripts {
            let img = compile_lua_source(src).expect("script should compile");
            assert_eq!(&img[0..4], b"LUA1");
            assert!(img.len() <= LUA_MAX_IMAGE_LEN);
        }
    }

    #[test]
    fn reports_syntax_errors_cleanly() {
        let res = compile_lua_source("function init(\n  led(0, 0, 15)\nend");
        assert!(res.is_err());
    }
}
